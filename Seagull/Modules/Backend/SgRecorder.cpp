#include "SgRecorder.h"
#include "SgPaths.h"

#include <QProcess>
#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QTimer>
#include <QPointer>

SgRecorder::SgRecorder(QObject* parent) : QObject(parent) {}

SgRecorder::~SgRecorder() {
    // App is closing — wrap up synchronously. Disconnect our lambdas first so they
    // don't fire (and touch a half-torn-down player) while we block in waitForFinished;
    // m_proc/m_clipProc are children of this, destroyed right after the body.
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->write("q\n");
            if (!m_proc->waitForFinished(1200)) m_proc->kill(); // container survives the kill
            m_proc->waitForFinished(1000);
        }
    }
    if (m_clipProc) {
        m_clipProc->disconnect(this);
        if (m_clipProc->state() != QProcess::NotRunning) {
            killTree(m_clipProc); // yt-dlp + its ffmpeg child
            m_clipProc->waitForFinished(1500);
        }
    }
}

bool SgRecorder::isRecording() const {
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

QString SgRecorder::outputFile() const {
    return isRecording() ? m_outFile : QString();
}

QString SgRecorder::ffmpegPath() const {
    return QCoreApplication::applicationDirPath() + "/tools/ffmpeg.exe";
}

QString SgRecorder::ytDlpPath() const {
    return QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
}

QString SgRecorder::toolsDir() const {
    return QCoreApplication::applicationDirPath() + "/tools";
}

QString SgRecorder::mergeFormat() const {
    // yt-dlp merges into mp4/mkv (not ts); fall back to mkv for anything else.
    return extForFormat() == "mp4" ? "mp4" : "mkv";
}

QString SgRecorder::outputDir() const {
    return SgPaths::recordingFolder();
}

bool SgRecorder::audioOnly() const {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    return cfg.value("Recording/Type", "Video").toString() == "Audio";
}

QString SgRecorder::extForFormat() const {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    if (audioOnly()) {
        const QString fmt = cfg.value("Recording/Format", "M4A").toString().toLower();
        if (fmt == "mp3" || fmt == "opus" || fmt == "flac" || fmt == "wav") return fmt;
        return "m4a"; // default — lossless AAC copy from nearly every stream
    }
    // Streaming/RecordFormat is the pre-Recording-settings key; honour it as the
    // fallback so existing configs keep their choice.
    const QString legacy = cfg.value("Streaming/RecordFormat", "MP4").toString();
    const QString fmt = cfg.value("Recording/Format", legacy).toString().toUpper();
    if (fmt == "MP4") return "mp4";
    if (fmt == "TS")  return "ts";
    return "mkv"; // unknown value — the safest container for an abrupt stop
}

QStringList SgRecorder::audioCodecArgs(const QString& ext) {
    // M4A copies the (near-universal AAC) stream losslessly; the other formats are
    // encoded live, which is cheap for audio.
    if (ext == "m4a")  return { "-c:a", "copy" };
    if (ext == "mp3")  return { "-c:a", "libmp3lame", "-q:a", "0" };
    if (ext == "opus") return { "-c:a", "libopus", "-b:a", "160k" };
    if (ext == "flac") return { "-c:a", "flac" };
    return { "-c:a", "pcm_s16le" }; // wav
}

QStringList SgRecorder::adtsFixArgs(const QString& ext) {
    // HLS/TS sources (Twitch especially) carry AAC as ADTS; copying that into an
    // MP4/M4A container needs it repacked to ASC or the muxer rejects the very
    // first audio packet ("Malformed AAC bitstream") and ffmpeg dies instantly.
    // The filter is a no-op for AAC that's already ASC, so it's safe for every
    // network source we record — but NOT for non-AAC audio (local-file clips
    // must skip it).
    if (ext == "mp4" || ext == "m4a") return { "-bsf:a", "aac_adtstoasc" };
    return {};
}

QString SgRecorder::sanitize(const QString& name) {
    QString s = name;
    s.replace(QRegularExpression("[<>:\"/\\\\|?*\\r\\n\\t]"), " ");
    s = s.simplified();
    if (s.size() > 120) s = s.left(120).trimmed();
    return s.isEmpty() ? QStringLiteral("stream") : s;
}

void SgRecorder::start(const QUrl& videoUrl, const QUrl& audioUrl,
    const QString& referer, const QString& title) {
    if (isRecording()) return;
    if (!videoUrl.isValid() || videoUrl.isEmpty()) {
        emit logMessage("Recorder: no stream URL to record.");
        return;
    }

    QDir().mkpath(outputDir());
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
    m_outFile = QDir(outputDir()).filePath(sanitize(title) + " " + stamp + "." + extForFormat());

    const bool hasAudio = audioUrl.isValid() && !audioUrl.isEmpty();
    const bool audio = audioOnly();
    const QString ext = extForFormat();
    const QString headers = referer.isEmpty() ? QString()
        : QString("Referer: %1\r\n").arg(referer);

    QStringList args;
    args << "-hide_banner" << "-loglevel" << "warning" << "-y";
    args << "-user_agent" << "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";

    if (audio && hasAudio) {
        // Audio-only with a separate audio stream: pull just it — no video bandwidth.
        if (!headers.isEmpty()) args << "-headers" << headers;
        args << "-i" << audioUrl.toString();
    } else {
        if (!headers.isEmpty()) args << "-headers" << headers;
        args << "-i" << videoUrl.toString();
        if (!audio && hasAudio) {
            if (!headers.isEmpty()) args << "-headers" << headers;
            args << "-i" << audioUrl.toString();
            args << "-map" << "0:v:0" << "-map" << "1:a:0";
        }
    }

    if (audio) {
        args << "-vn" << audioCodecArgs(ext);
    } else {
        args << "-c" << "copy";
    }
    args << adtsFixArgs(ext); // live sources are always network streams (AAC)
    // We stop by hard-killing ffmpeg (graceful 'q' is unreliable from a piped stdin
    // on Windows). MKV/TS already survive that; fragment MP4/M4A so they do too — an
    // empty moov up front means the file stays playable if the process is killed.
    // M4A has no video keyframes to fragment on, so it cuts on time instead.
    if (ext == "mp4")
        args << "-movflags" << "+frag_keyframe+empty_moov+default_base_moof";
    else if (ext == "m4a")
        args << "-movflags" << "+empty_moov+default_base_moof"
             << "-frag_duration" << "2000000"; // 2s fragments
    args << m_outFile;

    m_stopping = false;
    QProcess* p = new QProcess(this);    // capture this exact process in every lambda
    m_proc = p;
    p->setProcessChannelMode(QProcess::MergedChannels);

    connect(p, &QProcess::readyReadStandardOutput, this, [this, p]() {
        const QString out = QString::fromUtf8(p->readAll()).trimmed();
        if (!out.isEmpty()) emit logMessage("ffmpeg: " + out);
        });

    connect(p, &QProcess::started, this, [this]() {
        emit started(m_outFile);
        });

    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit logMessage("Recorder: failed to launch ffmpeg.");
            const QString file = m_outFile;
            if (m_proc == p) m_proc = nullptr;
            p->deleteLater();
            emit finished(file, false);
        }
        });

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, p](int code, QProcess::ExitStatus status) {
            // A user stop is a deliberate kill (m_stopping) and leaves a valid file.
            const bool ok = m_stopping || (status == QProcess::NormalExit && code == 0);
            const QString file = m_outFile;
            if (m_proc == p) m_proc = nullptr; // only clear if it's still the active one
            p->deleteLater();
            emit finished(file, ok);
        });

    p->start(ffmpegPath(), args);
}

bool SgRecorder::isClipping() const {
    return m_clipProc && m_clipProc->state() != QProcess::NotRunning;
}

void SgRecorder::cancelClip() {
    if (isClipping()) { m_clipCancelled = true; killTree(m_clipProc); }
}

void SgRecorder::killTree(QProcess* p) {
    // yt-dlp spawns ffmpeg as a child; QProcess::kill() would orphan it. taskkill /T
    // takes down the whole tree (Windows-only app).
    if (!p || p->state() == QProcess::NotRunning) return;
    QProcess::startDetached("taskkill", { "/PID", QString::number(p->processId()), "/T", "/F" });
}

// Single exit point for a clip job: tidy temp/partials and tell the UI we're done.
void SgRecorder::finishClip(const QString& file, bool ok) {
    if (!m_clipTempFile.isEmpty()) { QFile::remove(m_clipTempFile); m_clipTempFile.clear(); }
    QFile::remove(m_clipFile + ".part"); // leftover from a killed full download
    if (!ok && !m_clipFile.isEmpty()) QFile::remove(m_clipFile); // partial direct cut / trim
    m_clipStage = ClipStage::None;
    m_clipCancelled = false;
    m_clipSwitching = false;
    m_clipSlowHits = 0;
    emit clipFinished(file, ok);
}

void SgRecorder::clipSection(const QString& pageUrl, const QUrl& videoUrl, const QUrl& audioUrl,
    qint64 startMs, qint64 endMs, const QString& title) {
    // A local-file clip has no page URL — the file itself is the only source needed.
    const bool localSource = videoUrl.isLocalFile();
    if (isClipping() || endMs <= startMs || (pageUrl.isEmpty() && !localSource)) {
        emit clipFinished(QString(), false);
        return;
    }

    QDir().mkpath(outputDir());
    m_clipAudio = audioOnly(); // freeze the type for this clip — settings can change mid-save
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
    const QString base = sanitize(title) + " " + stamp + " CLIP";
    m_clipFile = QDir(outputDir()).filePath(base + "." + (m_clipAudio ? extForFormat() : mergeFormat()));
    m_clipTempFile.clear();
    m_clipPageUrl = pageUrl;
    m_clipVideoUrl = videoUrl;
    m_clipAudioUrl = audioUrl;
    m_clipStartMs = startMs;
    m_clipEndMs = endMs;
    m_clipTitle = title;
    m_clipYouTube = pageUrl.contains("youtube.com", Qt::CaseInsensitive)
        || pageUrl.contains("youtu.be", Qt::CaseInsensitive);
    m_clipCancelled = m_clipSwitching = false;
    m_clipSlowHits = 0;

    startClipSection();
}

void SgRecorder::startClipSection() {
    // Fast path: cut the section straight from the CDN URLs the player already
    // resolved (the exact streams VLC is playing) — a single ffmpeg with an input
    // seek, writing the FINAL clip file. No yt-dlp launch, no page re-resolve
    // (that alone used to cost 5-15s before the first byte downloaded).
    if (!m_clipVideoUrl.isValid() || m_clipVideoUrl.isEmpty()) {
        startClipFullDownload(); // no resolved stream on hand — go the yt-dlp route
        return;
    }

    m_clipStage = ClipStage::Section;
    const bool hasAudio = m_clipAudioUrl.isValid() && !m_clipAudioUrl.isEmpty();
    const QString headers = m_clipPageUrl.isEmpty() ? QString()
        : QString("Referer: %1\r\n").arg(m_clipPageUrl);
    const QString startS = QString::number(m_clipStartMs / 1000.0, 'f', 3);
    const QString durS = QString::number((m_clipEndMs - m_clipStartMs) / 1000.0, 'f', 3);

    QStringList args;
    // -progress pipe:1 gives machine-readable speed= lines for the throttle watchdog
    // (-loglevel warning would otherwise silence the stats line).
    args << "-hide_banner" << "-loglevel" << "warning" << "-nostats"
        << "-progress" << "pipe:1" << "-y";

    // Input options (UA/Referer/seek) must precede each -i they apply to.
    // -rw_timeout: a dead connection errors out (-> failure banner) instead of
    // hanging the grab forever — non-YouTube has no watchdog to rescue it.
    // A local file takes none of the network options — just the seek + native path.
    auto addInput = [&](const QUrl& u) {
        if (u.isLocalFile()) {
            args << "-ss" << startS << "-i" << QDir::toNativeSeparators(u.toLocalFile());
            return;
        }
        args << "-user_agent" << "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
             << "-rw_timeout" << "15000000"; // 15s, in microseconds
        if (!headers.isEmpty()) args << "-headers" << headers;
        args << "-ss" << startS << "-i" << u.toString();
    };

    if (m_clipAudio) {
        addInput(hasAudio ? m_clipAudioUrl : m_clipVideoUrl);
        args << "-vn" << audioCodecArgs(QFileInfo(m_clipFile).suffix());
    } else {
        addInput(m_clipVideoUrl);
        if (hasAudio) {
            addInput(m_clipAudioUrl);
            args << "-map" << "0:v:0" << "-map" << "1:a:0";
        }
        args << "-c" << "copy";
    }
    // Same ADTS->ASC repack as live recording, but network sources only — a
    // local file's audio may not be AAC, and the filter rejects other codecs.
    if (!m_clipVideoUrl.isLocalFile())
        args << adtsFixArgs(QFileInfo(m_clipFile).suffix());
    args << "-t" << durS << m_clipFile;

    QProcess* cp = new QProcess(this);
    m_clipProc = cp;
    cp->setProcessChannelMode(QProcess::MergedChannels);
    m_clipClock.restart();

    connect(cp, &QProcess::readyReadStandardOutput, this, [this, cp]() {
        const QString out = QString::fromUtf8(cp->readAll());
        // -progress chatter is too noisy to log; only surface real warnings/errors.
        const QString trimmed = out.trimmed();
        if (!trimmed.isEmpty() && !trimmed.contains("progress="))
            emit logMessage("clip: " + trimmed);
        // Watchdog (YouTube only): its nsig throttling can drag the cut below
        // realtime, where the parallel full download + trim wins. Other sites
        // serve the section at full speed (especially with playback paused), so
        // the direct cut is the whole story there — never escalate. The first
        // seconds are ramp-up (low speed while the seek settles) — grace period.
        if (!m_clipYouTube) return;
        if (m_clipStage != ClipStage::Section || m_clipSwitching) return;
        if (m_clipClock.elapsed() < 8000) return;
        static const QRegularExpression re("speed=\\s*([0-9.]+)x");
        auto it = re.globalMatch(out);
        while (it.hasNext()) {
            const double speed = it.next().captured(1).toDouble();
            m_clipSlowHits = (speed < 0.6) ? m_clipSlowHits + 1 : 0;
        }
        if (m_clipSlowHits >= 3) switchClipToFull();
        });
    connect(cp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, cp](int code, QProcess::ExitStatus status) {
            if (m_clipProc == cp) m_clipProc = nullptr;
            cp->deleteLater();
            if (m_clipCancelled) { finishClip(QString(), false); return; }       // cancel wins
            if (m_clipSwitching) { m_clipSwitching = false; startClipFullDownload(); return; }
            if (status == QProcess::NormalExit && code == 0) { finishClip(m_clipFile, true); return; }
            // Direct grab failed (expired URL, 403, …). YouTube falls back to a
            // fresh yt-dlp full download + trim; other sites keep it simple — the
            // direct cut is the only path, so report the failure.
            if (m_clipYouTube) {
                emit logMessage("clip: direct section cut failed — falling back to full download + trim.");
                QFile::remove(m_clipFile); // drop the partial; the trim rewrites it
                startClipFullDownload();
            } else {
                emit logMessage("clip: direct section cut failed.");
                finishClip(QString(), false);
            }
        });
    connect(cp, &QProcess::errorOccurred, this, [this, cp](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { if (m_clipProc == cp) m_clipProc = nullptr; cp->deleteLater(); finishClip(QString(), false); }
        });

    cp->start(ffmpegPath(), args);

    // Wall-clock fallback (YouTube only, like the watchdog) for stalls the speed
    // parser misses: if the cut is still running well past the clip's own
    // duration, give up on it and go full download + trim.
    if (m_clipYouTube) {
        const qint64 budgetMs = qMax<qint64>(45000, (m_clipEndMs - m_clipStartMs) * 2);
        QPointer<QProcess> guard(cp);
        QTimer::singleShot(budgetMs, this, [this, guard]() {
            if (guard && m_clipProc == guard && m_clipStage == ClipStage::Section)
                switchClipToFull();
            });
    }
}

void SgRecorder::switchClipToFull() {
    if (m_clipSwitching) return;
    m_clipSwitching = true;
    emit logMessage("clip: section throttled — switching to concurrent full download + trim.");
    killTree(m_clipProc); // its finished handler then calls startClipFullDownload()
}

void SgRecorder::startClipFullDownload() {
    m_clipStage = ClipStage::FullDownload;
    const QString tempBase = QFileInfo(m_clipFile).completeBaseName() + ".full";
    const QString clipExt = QFileInfo(m_clipFile).suffix();
    m_clipTempFile = QDir(outputDir()).filePath(tempBase + "." + clipExt);

    QStringList args;
    args << "--no-warnings" << "--no-playlist"
        << "-N" << "16"                              // concurrent fragments beat the throttle
        << "--ffmpeg-location" << toolsDir()
        << "-o" << QDir(outputDir()).filePath(tempBase + ".%(ext)s");
    if (m_clipAudio)
        args << "-f" << "ba/b" << "-x" << "--audio-format" << clipExt;
    else
        args << "-f" << "bv*+ba/b" << "--merge-output-format" << clipExt;
    if (!m_clipYouTube) args << "--impersonate" << "chrome";
    args << m_clipPageUrl;

    QProcess* cp = new QProcess(this);
    m_clipProc = cp;
    cp->setProcessChannelMode(QProcess::MergedChannels);
    connect(cp, &QProcess::readyReadStandardOutput, this, [this, cp]() {
        const QString out = QString::fromUtf8(cp->readAll()).trimmed();
        if (!out.isEmpty()) emit logMessage("clip(dl): " + out);
        });
    connect(cp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, cp](int code, QProcess::ExitStatus status) {
            if (m_clipProc == cp) m_clipProc = nullptr;
            cp->deleteLater();
            if (m_clipCancelled || status != QProcess::NormalExit || code != 0) { finishClip(QString(), false); return; }
            startClipTrim();
        });
    connect(cp, &QProcess::errorOccurred, this, [this, cp](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { if (m_clipProc == cp) m_clipProc = nullptr; cp->deleteLater(); finishClip(QString(), false); }
        });
    cp->start(ytDlpPath(), args);
}

void SgRecorder::startClipTrim() {
    m_clipStage = ClipStage::Trim;
    QStringList args;
    // -ss before -i = fast keyframe seek; -t is an unambiguous duration from there.
    args << "-hide_banner" << "-loglevel" << "warning" << "-y"
        << "-ss" << QString::number(m_clipStartMs / 1000.0, 'f', 3)
        << "-i" << m_clipTempFile
        << "-t" << QString::number((m_clipEndMs - m_clipStartMs) / 1000.0, 'f', 3)
        << "-c" << "copy" << m_clipFile;

    QProcess* cp = new QProcess(this);
    m_clipProc = cp;
    cp->setProcessChannelMode(QProcess::MergedChannels);
    connect(cp, &QProcess::readyReadStandardOutput, this, [this, cp]() {
        const QString out = QString::fromUtf8(cp->readAll()).trimmed();
        if (!out.isEmpty()) emit logMessage("clip(trim): " + out);
        });
    connect(cp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, cp](int code, QProcess::ExitStatus status) {
            if (m_clipProc == cp) m_clipProc = nullptr;
            cp->deleteLater();
            finishClip(m_clipFile, !m_clipCancelled && status == QProcess::NormalExit && code == 0);
        });
    connect(cp, &QProcess::errorOccurred, this, [this, cp](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { if (m_clipProc == cp) m_clipProc = nullptr; cp->deleteLater(); finishClip(QString(), false); }
        });
    cp->start(ffmpegPath(), args);
}

void SgRecorder::stop() {
    if (!isRecording()) return;
    m_stopping = true;
    QProcess* p = m_proc;

    // Best-effort graceful first (works on some builds), but don't wait long: on
    // Windows 'q' usually doesn't reach a piped ffmpeg and terminate() is a no-op
    // for a console app. Our containers survive a hard kill, so escalate quickly so
    // the button/state respond promptly. finished() fires when it exits.
    p->write("q\n");

    QPointer<QProcess> pp = p; // guard against it being freed before the timer fires
    QTimer::singleShot(1200, this, [pp]() {
        if (pp && pp->state() != QProcess::NotRunning) pp->kill();
        });
}
