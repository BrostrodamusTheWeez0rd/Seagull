#include "SgYtDlp.h"
#include "SgOptions.h"
#include "SgHlsProxy.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QLocale>
#include <QDate>
#include <QDateTime>

namespace {
    bool isYoutubeUrl(const QString& url) {
        const QString u = url.toLower();
        return u.contains("youtube.com") || u.contains("youtu.be");
    }
    // Non-YouTube sites are often bot-protected (TLS fingerprinting): impersonate a
    // browser so they don't reject yt-dlp with HTTP 403/410. YouTube doesn't need it.
    void addImpersonateIfNeeded(QStringList& args, const QString& url) {
        if (!isYoutubeUrl(url)) args << "--impersonate" << "chrome";
    }

    // Classify a yt-dlp failure as the source blocking us. Returns "bot" (a
    // bot/sign-in challenge), "throttle" (rate-limit / HTTP 429), or "" (neither —
    // an ordinary failure we don't want to nag the user about).
    QString classifyBlock(const QString& errText) {
        const QString e = errText.toLower();
        // Rate-limiting / throttling first: a 429 sometimes rides alongside other noise.
        if (e.contains("429") || e.contains("too many requests")
            || e.contains("throttl") || e.contains("rate-limit") || e.contains("rate limit"))
            return QStringLiteral("throttle");
        // Bot / sign-in challenge. Match on "not a bot" / "suspicion of bot" — the
        // bot message always contains one. (Deliberately NOT "confirm you", which
        // would also catch the unrelated "Sign in to confirm your age" age gate.)
        if (e.contains("not a bot") || e.contains("suspicion of bot"))
            return QStringLiteral("bot");
        return QString();
    }
}

SgYtDlp::SgYtDlp(QObject* parent) : QObject(parent) {
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyReadStandardOutput,
        this, &SgYtDlp::handleReadyRead);

    connect(m_process, &QProcess::finished,
        this, &SgYtDlp::handleProcessFinished);

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit logMessage("CRITICAL ERROR: Could not find yt-dlp.exe!");
            emit logMessage("Looked in: " + QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe");
        }
        });
}

SgYtDlp::~SgYtDlp() {
    cancel();
}

void SgYtDlp::download(const QString& url) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::Downloading;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
    QStringList args = SgOptions::buildDownloadArgs(url); // ends with the url
    if (!isYoutubeUrl(url)) {
        const QString u = args.takeLast();                // pull the url off the end
        args << "--impersonate" << "chrome" << u;         // ...put impersonate before it
    }

    emit logMessage("Starting download: " + url);
    m_process->start(exePath, args);
}

void SgYtDlp::fetchMetadataAndStreamUrl(const QString& url, const QString& formatId, bool freshResolve) {
    if (freshResolve) m_metaCache.remove(url); // the cached stream URL went stale

    // A fresh-enough earlier resolve answers instantly: quality switches re-pair
    // from the cached format list and replays skip the whole yt-dlp launch.
    const QJsonObject cached = cachedMetadata(url);
    if (!cached.isEmpty()) {
        emit logMessage("Metadata cache hit — resolving stream locally: " + url);
        m_pendingFormatId = formatId;
        processMetadata(cached);
        return;
    }

    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingMetadata;
    processBuffer.clear();

    // Remember which quality the caller wants. We resolve the full format list
    // with -J (no -f) and do container-matched A/V pairing ourselves in
    // handleProcessFinished, so VLC never gets a mismatched video+audio pair.
    m_pendingFormatId = formatId;
    m_pendingMetaUrl = url;

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    // --no-playlist: a video URL carrying a &list= param would otherwise resolve
    // the whole playlist before this video can start (can take minutes).
    args << "-J" << "--quiet" << "--no-warnings" << "--no-playlist";
    addImpersonateIfNeeded(args, url);
    args << url;

    emit logMessage("Fetching metadata for: " + url
        + (formatId.isEmpty() ? QString("  [format: default]")
                              : QString("  [format: %1]").arg(formatId)));
    m_process->start(exePath, args);
}

void SgYtDlp::probeAvailableQualities(const QString& url) {
    const QJsonObject cached = cachedMetadata(url);
    if (!cached.isEmpty()) {
        emitProbeResults(cached);
        return;
    }

    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::Probing;
    processBuffer.clear();
    m_pendingMetaUrl = url;

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--quiet" << "--no-warnings" << "--no-playlist";
    addImpersonateIfNeeded(args, url);
    args << url;

    emit logMessage("Probing qualities for: " + url);
    m_process->start(exePath, args);
}

QJsonObject SgYtDlp::cachedMetadata(const QString& url) {
    constexpr qint64 kTtlMs = 30 * 60 * 1000; // CDN URLs comfortably outlive this
    const auto it = m_metaCache.constFind(url);
    if (it == m_metaCache.constEnd()) return {};
    if (QDateTime::currentMSecsSinceEpoch() - it->atMs > kTtlMs) {
        m_metaCache.remove(url);
        return {};
    }
    return it->obj;
}

void SgYtDlp::storeMetadata(const QString& url, const QJsonObject& obj) {
    if (url.isEmpty() || obj["is_live"].toBool()) return; // live URLs rotate — never cache
    if (m_metaCache.size() > 16) m_metaCache.clear();     // tiny working set; keep it bounded
    m_metaCache.insert(url, { obj, QDateTime::currentMSecsSinceEpoch() });
}

void SgYtDlp::fetchPlaylistEntries(const QString& playlistUrl) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingPlaylist;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings";
    addImpersonateIfNeeded(args, playlistUrl);
    args << playlistUrl;

    emit logMessage("Fetching playlist entries...");
    m_process->start(exePath, args);
}

void SgYtDlp::cancel() {
    if (m_process->state() == QProcess::Running) {
        m_process->kill();
        m_process->waitForFinished();
    }

    currentMode = JobMode::Idle;
    processBuffer.clear();
}

void SgYtDlp::handleReadyRead() {
    QByteArray output = m_process->readAllStandardOutput();

    if (currentMode == JobMode::FetchingMetadata ||
        currentMode == JobMode::Probing ||
        currentMode == JobMode::FetchingPlaylist) {

        processBuffer.append(output);
        return;
    }

    if (currentMode == JobMode::Downloading) {
        QString text = QString::fromLocal8Bit(output).trimmed();
        QStringList lines = text.split('\n');

        static QRegularExpression progressRegex("\\[download\\]\\s+([0-9.]+)\\%");

        for (const QString& line : lines) {
            QString cleanLine = line.trimmed();
            if (cleanLine.isEmpty()) continue;

            emit logMessage(cleanLine);

            QRegularExpressionMatch match = progressRegex.match(cleanLine);
            if (match.hasMatch()) {
                emit progressUpdated(match.captured(1).toDouble());
            } else {
                // Keep the non-progress lines (errors/warnings) so a failed download
                // can still be classified as a bot/throttle block at the end.
                processBuffer.append(cleanLine.toLocal8Bit()).append('\n');
            }
        }
    }
}

void SgYtDlp::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    JobMode mode = currentMode;
    currentMode = JobMode::Idle;

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        emit logMessage("Operation failed (yt-dlp). Code: " + QString::number(exitCode));
        // Surface yt-dlp's own error text (merged stdout+stderr) so failures like a
        // broken extractor / age gate are diagnosable instead of silently swallowed.
        const QString errOut = QString::fromLocal8Bit(processBuffer).trimmed();
        if (!errOut.isEmpty())
            emit logMessage("yt-dlp: " + errOut.right(800));
        // If the source is blocking us (bot challenge / throttling), let the
        // orchestrator warn the user — these aren't fixable by a retry.
        const QString block = classifyBlock(errOut);
        if (!block.isEmpty())
            emit extractionBlocked(block, errOut.right(400));
        emit finished(false);
        return;
    }

    if (mode == JobMode::Downloading) {
        emit finished(exitCode == 0);
        return;
    }

    int jsonStart = processBuffer.indexOf('{');
    if (jsonStart == -1) {
        emit logMessage("No valid JSON found in output.");
        emit finished(false);
        return;
    }
    QByteArray jsonData = processBuffer.mid(jsonStart);

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(jsonData, &err);

    if (err.error != QJsonParseError::NoError || doc.isNull()) {
        emit logMessage("JSON parse failed: " + err.errorString());
        emit finished(false);
        return;
    }

    if (mode == JobMode::FetchingPlaylist) {
        QList<QString> urls;
        QJsonArray entries = doc.object()["entries"].toArray();
        for (const auto& entry : entries)
            urls.append(entry.toObject()["url"].toString());

        emit playlistEntriesReady(urls);
    }

    else if (mode == JobMode::Probing) {
        const QJsonObject root = doc.object();
        storeMetadata(m_pendingMetaUrl, root);
        m_pendingMetaUrl.clear();
        emitProbeResults(root);
    }

    else if (mode == JobMode::FetchingMetadata) {
        const QJsonObject obj = doc.object();
        storeMetadata(m_pendingMetaUrl, obj);
        m_pendingMetaUrl.clear();
        processMetadata(obj);
    }
}

// The probe runs on every play, so surface the poster thumbnail + the full
// metadata (for the player's Info panel) here too.
void SgYtDlp::emitProbeResults(const QJsonObject& root) {
    emit thumbnailResolved(SgFormat::pickThumbnail(root));
    emit availableQualitiesFound(SgFormat::buildQualityOptions(root));
    emit liveStatusKnown(root["is_live"].toBool());

    QString vdate = root["upload_date"].toString();
    QDate pd = QDate::fromString(vdate, "yyyyMMdd");
    if (pd.isValid()) vdate = pd.toString("MMM d, yyyy");
    QString vuploader = root["uploader"].toString();
    if (vuploader.isEmpty()) vuploader = root["channel"].toString(); // some sites only set "channel"
    emit videoInfoReady(
        root["title"].toString(),
        vuploader,
        QLocale(QLocale::English).toString(root["view_count"].toInt()),
        vdate,
        root["description"].toString());
}

void SgYtDlp::processMetadata(const QJsonObject& obj) {
    QString title = obj["title"].toString();
    QString uploader = obj["uploader"].toString();
    if (uploader.isEmpty()) uploader = obj["channel"].toString(); // some sites only set "channel"
    QString duration = obj["duration_string"].toString();
    QString viewCount = QLocale(QLocale::English).toString(obj["view_count"].toInt());
    // yt-dlp gives upload_date as a bare "YYYYMMDD" string — make it readable.
    QString uploadDate = obj["upload_date"].toString();
    QDate parsedDate = QDate::fromString(uploadDate, "yyyyMMdd");
    if (parsedDate.isValid())
        uploadDate = parsedDate.toString("MMM d, yyyy");

    emit metadataReady(title, uploader, duration, viewCount, uploadDate, SgFormat::pickThumbnail(obj));

    // Feed the player's quality menu / thumbnail / info / live badge from this
    // same job, so a play-by-URL with no prefetched CDN (e.g. a Search result)
    // gets everything from one resolve instead of a separate probe contending
    // for this worker. (Unconnected for the Queue workers — harmless there.)
    emitProbeResults(obj);

    // Resolve a playable stream. YouTube gets a container-matched video+audio
    // pair (VLC input-slave merge); every other site gets a single muxed
    // stream. Cap to the requested height (or the default Stream Quality
    // setting when no explicit format was chosen).
    const QString wantId = m_pendingFormatId;
    m_pendingFormatId.clear();

    QJsonArray formats = obj["formats"].toArray();
    int targetH = wantId.isEmpty() ? SgOptions::defaultStreamHeight()
                                   : SgFormat::heightForFormatId(formats, wantId);

    emit logMessage("Resolving stream [extractor: " + obj["extractor_key"].toString() + "]");

    QString videoUrl, audioUrl;
    if (SgFormat::resolveStream(obj, targetH, videoUrl, audioUrl)) {
        // Twitch stitches ads server-side. yt-dlp resolves with playerType="site"
        // (ad-served); the proxy instead resolves its OWN PlaybackAccessToken with
        // playerType="embed" (clean stream) and serves that, ad-stripping yt-dlp's
        // URL only as a fallback. (Only Twitch live; every other site / VOD untouched.)
        const bool isTwitch = obj["extractor_key"].toString().startsWith("twitch", Qt::CaseInsensitive)
            || obj["extractor"].toString().startsWith("twitch", Qt::CaseInsensitive);
        if (m_hlsProxy && isTwitch && obj["is_live"].toBool() && audioUrl.isEmpty()) {
            QString login = obj["uploader_id"].toString();
            if (login.isEmpty()) login = obj["display_id"].toString();
            if (!login.isEmpty()) {
                videoUrl = m_hlsProxy->proxifyTwitch(login, targetH, QUrl(videoUrl), "https://www.twitch.tv/").toString();
                emit logMessage("Twitch live: resolving ad-free stream via embed token (login=" + login + ").");
            }
        }
        emit logMessage(QString("Stream resolved%1: %2")
            .arg(audioUrl.isEmpty() ? QString() : QString(" (+separate audio)"), videoUrl.left(160)));
        emit streamUrlReady(QUrl(videoUrl), audioUrl.isEmpty() ? QUrl() : QUrl(audioUrl));
    }
    else
        emit logMessage("No playable stream format found.");
}
