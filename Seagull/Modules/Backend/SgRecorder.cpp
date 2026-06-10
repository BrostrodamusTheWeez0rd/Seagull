#include "SgRecorder.h"

#include <QProcess>
#include <QCoreApplication>
#include <QSettings>
#include <QDir>
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
            m_clipProc->kill();
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
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    return cfg.value("Paths/DownloadFolder",
        QCoreApplication::applicationDirPath() + "/Downloads").toString();
}

QString SgRecorder::extForFormat() const {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    const QString fmt = cfg.value("Streaming/RecordFormat", "MKV").toString().toUpper();
    if (fmt == "MP4") return "mp4";
    if (fmt == "TS")  return "ts";
    return "mkv"; // default + safest for an abrupt stop
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
    const QString headers = referer.isEmpty() ? QString()
        : QString("Referer: %1\r\n").arg(referer);

    QStringList args;
    args << "-hide_banner" << "-loglevel" << "warning" << "-y";
    args << "-user_agent" << "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";

    if (!headers.isEmpty()) args << "-headers" << headers;
    args << "-i" << videoUrl.toString();

    if (hasAudio) {
        if (!headers.isEmpty()) args << "-headers" << headers;
        args << "-i" << audioUrl.toString();
        args << "-map" << "0:v:0" << "-map" << "1:a:0";
    }

    args << "-c" << "copy";
    // We stop by hard-killing ffmpeg (graceful 'q' is unreliable from a piped stdin
    // on Windows). MKV/TS already survive that; fragment MP4 so it does too — an
    // empty moov up front means the file stays playable if the process is killed.
    if (extForFormat() == "mp4")
        args << "-movflags" << "+frag_keyframe+empty_moov+default_base_moof";
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
    if (isClipping()) { m_clipCancelled = true; m_clipProc->kill(); }
}

void SgRecorder::clipSection(const QString& pageUrl, qint64 startMs, qint64 endMs, const QString& title) {
    if (isClipping() || pageUrl.isEmpty() || endMs <= startMs) {
        emit clipFinished(QString(), false);
        return;
    }

    QDir().mkpath(outputDir());
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
    const QString base = sanitize(title) + " clip " + stamp;
    // yt-dlp fills %(ext)s; mergeFormat() pins the container so we know the path.
    m_clipFile = QDir(outputDir()).filePath(base + "." + mergeFormat());

    const QString section = QString("*%1-%2")
        .arg(startMs / 1000.0, 0, 'f', 3).arg(endMs / 1000.0, 0, 'f', 3);

    QStringList args;
    args << "--download-sections" << section
        << "--no-warnings" << "--no-playlist"
        << "-f" << "bv*+ba/b"
        << "--remux-video" << mergeFormat()  // pin the final container (no re-encode)
        << "--ffmpeg-location" << toolsDir()
        << "-o" << QDir(outputDir()).filePath(base + ".%(ext)s");
    // Non-YouTube sites are bot-protected; impersonate a browser like the rest of the app.
    if (!pageUrl.contains("youtube.com", Qt::CaseInsensitive)
        && !pageUrl.contains("youtu.be", Qt::CaseInsensitive))
        args << "--impersonate" << "chrome";
    args << pageUrl;

    qDebug() << "[clip] yt-dlp" << args.join(' ');

    QProcess* cp = new QProcess(this);
    m_clipProc = cp;
    cp->setProcessChannelMode(QProcess::MergedChannels);
    connect(cp, &QProcess::readyReadStandardOutput, this, [this, cp]() {
        const QString out = QString::fromUtf8(cp->readAll()).trimmed();
        if (!out.isEmpty()) { qDebug() << "[clip]" << out; emit logMessage("clip: " + out); }
        });
    connect(cp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, cp](int code, QProcess::ExitStatus status) {
            const bool ok = (status == QProcess::NormalExit && code == 0);
            qDebug() << "[clip] finished code:" << code << "status:" << status << "ok:" << ok;
            const QString file = m_clipFile;
            const bool cancelled = m_clipCancelled;
            m_clipCancelled = false;
            if (m_clipProc == cp) m_clipProc = nullptr;
            cp->deleteLater();
            if (!cancelled) emit clipFinished(file, ok); // a deliberate cancel stays quiet
        });
    connect(cp, &QProcess::errorOccurred, this, [this, cp](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            if (m_clipProc == cp) m_clipProc = nullptr;
            cp->deleteLater();
            emit clipFinished(QString(), false);
        }
        });

    cp->start(ytDlpPath(), args);
}

void SgRecorder::stop() {
    if (!isRecording()) return;
    m_stopping = true;
    // Best-effort graceful first (works on some builds), but don't wait long: on
    // Windows 'q' usually doesn't reach a piped ffmpeg and terminate() is a no-op
    // for a console app. Our containers survive a hard kill, so escalate quickly so
    // the button/state respond promptly. finished() fires when it exits.
    QProcess* p = m_proc;
    p->write("q\n");

    QPointer<QProcess> pp = p; // guard against it being freed before the timer fires
    QTimer::singleShot(1200, this, [pp]() {
        if (pp && pp->state() != QProcess::NotRunning) pp->kill();
        });
}
