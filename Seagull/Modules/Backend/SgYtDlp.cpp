#include "SgYtDlp.h"
#include <QCoreApplication>
#include <QSettings>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QLocale>

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
    QStringList args = buildDownloadArgs(url);

    emit logMessage("Starting download: " + url);
    m_process->start(exePath, args);
}

void SgYtDlp::fetchMetadataAndStreamUrl(const QString& url, const QString& formatId) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingMetadata;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--quiet" << "--no-warnings";

    if (formatId.isEmpty())
        args << "-f" << "bestvideo+bestaudio/best";
    else
        args << "-f" << QString("%1+bestaudio/best").arg(formatId);

    args << url;

    emit logMessage("Fetching metadata for: " + url);
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

QStringList SgYtDlp::buildDownloadArgs(const QString& url) {
    QStringList args;

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini",
        QSettings::IniFormat);

    QString format = settings.value("Download/Format", "Best Available").toString();
    QString quality = settings.value("Download/Quality", "Best Available").toString();
    QString dlFolder = settings.value("Paths/DownloadFolder",
        QCoreApplication::applicationDirPath() + "/Downloads").toString();

    args << "--newline"
        << "-o" << dlFolder + "/%(title)s.%(ext)s";

    if (format == "mp3" || format == "m4a") {
        args << "-x" << "--audio-format" << format;

        if (quality.contains("kbps")) {
            args << "--audio-quality" << quality.split(" ").first() + "K";
        }
    }
    else {
        if (quality == "Best Available" || format == "Best Available") {
            args << "-f" << "bestvideo+bestaudio/best";
        }
        else {
            QString res = quality.remove("p").split(" ").first();
            args << "-f" << QString("bestvideo[height<=%1]+bestaudio/best").arg(res);
        }

        if (format != "Best Available") {
            args << "--merge-output-format" << format;
        }
    }

    args << url;
    return args;
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

    if (exitStatus == QProcess::CrashExit) {
        emit logMessage("Operation failed: yt-dlp crashed.");
        emit finished(false);
        return;
    }

    if (processBuffer.isEmpty()) {
        emit logMessage("yt-dlp returned empty output.");
        emit finished(false);
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(processBuffer, &err);

    if (err.error != QJsonParseError::NoError || doc.isNull()) {
        emit logMessage("JSON parse failed: " + err.errorString());
        emit logMessage("Raw buffer (truncated): " + QString::fromUtf8(processBuffer.left(300)));
        emit finished(false);
        return;
    }

    if (mode == JobMode::FetchingPlaylist) {
        QList<QString> urls;
        QJsonArray entries = doc.object()["entries"].toArray();

        for (const auto& entry : entries) {
            urls.append(entry.toObject()["url"].toString());
        }

        emit playlistEntriesReady(urls);
    }

    else if (mode == JobMode::Probing) {
        QJsonArray formats = doc.object()["formats"].toArray();
        QList<StreamOption> options;
        QList<int> seenHeights;

        options.append({ "", "Auto", false });

        for (int i = formats.size() - 1; i >= 0; --i) {
            QJsonObject fmt = formats[i].toObject();
            int height = fmt["height"].toInt();
            QString vcodec = fmt["vcodec"].toString();

            if (height > 0 && vcodec != "none" && !seenHeights.contains(height)) {
                seenHeights.append(height);

                options.append({
                    fmt["format_id"].toString(),
                    QString::number(height) + "p",
                    false
                    });
            }
        }

        emit availableQualitiesFound(options);
    }

    else if (mode == JobMode::FetchingMetadata) {
        QJsonObject obj = doc.object();

        QString title = obj["title"].toString();
        QString uploader = obj["uploader"].toString();
        QString duration = obj["duration_string"].toString();
        QString viewCount = QLocale(QLocale::English).toString(obj["view_count"].toInt());
        QString uploadDate = obj["upload_date"].toString();
        QString thumb = obj["thumbnail"].toString();

        QString streamUrl = obj["url"].toString();
        QString audioUrl;

        if (streamUrl.isEmpty() && obj.contains("requested_formats")) {
            QJsonArray reqFormats = obj["requested_formats"].toArray();

            if (!reqFormats.isEmpty()) {
                streamUrl = reqFormats[0].toObject()["url"].toString();

                if (reqFormats.size() > 1) {
                    audioUrl = reqFormats[1].toObject()["url"].toString();
                }
            }
        }

        emit metadataReady(title, uploader, duration, viewCount, uploadDate, thumb);

        if (!streamUrl.isEmpty()) {
            emit streamUrlReady(QUrl(streamUrl),
                audioUrl.isEmpty() ? QUrl() : QUrl(audioUrl));
        }
    }

    else if (mode == JobMode::Downloading) {
        emit finished(exitCode == 0);
    }
}