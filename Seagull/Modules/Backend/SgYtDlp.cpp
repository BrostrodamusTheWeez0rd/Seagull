#include "SgYtDlp.h"
#include "SgOptions.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QLocale>
#include <QDate>

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
    QStringList args = SgOptions::buildDownloadArgs(url);

    emit logMessage("Starting download: " + url);
    m_process->start(exePath, args);
}

void SgYtDlp::fetchMetadataAndStreamUrl(const QString& url, const QString& formatId) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingMetadata;
    processBuffer.clear();

    // Remember which quality the caller wants. We resolve the full format list
    // with -J (no -f) and do container-matched A/V pairing ourselves in
    // handleProcessFinished, so VLC never gets a mismatched video+audio pair.
    m_pendingFormatId = formatId;

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--quiet" << "--no-warnings" << url;

    emit logMessage("Fetching metadata for: " + url
        + (formatId.isEmpty() ? QString("  [format: default]")
                              : QString("  [format: %1]").arg(formatId)));
    m_process->start(exePath, args);
}

void SgYtDlp::probeAvailableQualities(const QString& url) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::Probing;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--quiet" << "--no-warnings" << url;

    emit logMessage("Probing qualities for: " + url);
    m_process->start(exePath, args);
}

void SgYtDlp::fetchPlaylistEntries(const QString& playlistUrl) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingPlaylist;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings" << playlistUrl;

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
            }
        }
    }
}

void SgYtDlp::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    JobMode mode = currentMode;
    currentMode = JobMode::Idle;

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        emit logMessage("Operation failed (yt-dlp). Code: " + QString::number(exitCode));
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
        QJsonObject root = doc.object();
        // The probe runs on every play, so surface the poster thumbnail here too.
        emit thumbnailResolved(SgFormat::pickThumbnail(root));
        emit availableQualitiesFound(SgFormat::buildQualityOptions(root));
    }

    else if (mode == JobMode::FetchingMetadata) {
        QJsonObject obj = doc.object();

        QString title = obj["title"].toString();
        QString uploader = obj["uploader"].toString();
        QString duration = obj["duration_string"].toString();
        QString viewCount = QLocale(QLocale::English).toString(obj["view_count"].toInt());
        // yt-dlp gives upload_date as a bare "YYYYMMDD" string — make it readable.
        QString uploadDate = obj["upload_date"].toString();
        QDate parsedDate = QDate::fromString(uploadDate, "yyyyMMdd");
        if (parsedDate.isValid())
            uploadDate = parsedDate.toString("MMM d, yyyy");

        emit metadataReady(title, uploader, duration, viewCount, uploadDate, SgFormat::pickThumbnail(obj));

        // Pick a container-matched video+audio pair so VLC's input-slave merge
        // doesn't desync or drop audio. Cap to the requested height (or the
        // default Stream Quality setting when no explicit format was chosen).
        const QString wantId = m_pendingFormatId;
        m_pendingFormatId.clear();

        QJsonArray formats = obj["formats"].toArray();
        int targetH = wantId.isEmpty() ? SgOptions::defaultStreamHeight()
                                       : SgFormat::heightForFormatId(formats, wantId);

        QString videoUrl, audioUrl;
        if (SgFormat::chooseMatchedAvPair(formats, targetH, videoUrl, audioUrl)) {
            emit streamUrlReady(QUrl(videoUrl), QUrl(audioUrl));
        }
        else {
            // No split A/V pair available — fall back to a single progressive
            // stream (already muxed, no input-slave needed).
            QString prog = obj["url"].toString();
            if (prog.isEmpty()) prog = SgFormat::bestProgressiveUrl(formats);
            if (!prog.isEmpty())
                emit streamUrlReady(QUrl(prog), QUrl());
            else
                emit logMessage("No playable stream format found.");
        }
    }
}
