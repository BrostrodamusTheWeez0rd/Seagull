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
    // We merge channels so we can read standard download output easily
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &SgYtDlp::handleReadyRead);
    connect(m_process, &QProcess::finished, this, &SgYtDlp::handleProcessFinished);

    // Sanity check: Catches if the executable is missing or blocked
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

void SgYtDlp::fetchMetadataAndStreamUrl(const QString& url) {
    if (m_process->state() == QProcess::Running) return;

    currentMode = JobMode::FetchingMetadata;
    processBuffer.clear();

    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    // Restored: Force a combined, Qt-compatible MP4 stream so the player doesn't choke
    QStringList args;
    args << "-J" << "--quiet" << "--no-warnings" << "-f" << "b[ext=mp4]/best" << url;

    emit logMessage("Fetching metadata for: " + url);
    m_process->start(exePath, args);
}

void SgYtDlp::cancel() {
    if (m_process->state() == QProcess::Running) {
        emit logMessage("Cancelling operation...");
        m_process->kill();
        m_process->waitForFinished();
    }
    currentMode = JobMode::Idle;
}

QStringList SgYtDlp::buildDownloadArgs(const QString& url) {
    QStringList args;
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    QString format = settings.value("Download/Format", "Best Available").toString();
    QString quality = settings.value("Download/Quality", "Best Available").toString();
    QString dlFolder = settings.value("Paths/DownloadFolder", QCoreApplication::applicationDirPath() + "/Downloads").toString();

    args << "--newline" << "-o" << dlFolder + "/%(title)s.%(ext)s";

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
        if (format != "Best Available") args << "--merge-output-format" << format;
    }
    args << url;
    return args;
}

void SgYtDlp::handleReadyRead() {
    QByteArray output = m_process->readAllStandardOutput();

    if (currentMode == JobMode::FetchingMetadata) {
        // Accumulate output; JSON can be massive and arrive in chunks
        processBuffer.append(output);
    }
    else if (currentMode == JobMode::Downloading) {
        // Parse progress line-by-line
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
    if (exitStatus == QProcess::CrashExit) {
        emit logMessage("Operation failed: yt-dlp crashed.");
        emit finished(false);
        currentMode = JobMode::Idle;
        return;
    }

    if (currentMode == JobMode::FetchingMetadata) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(processBuffer, &parseError);

        if (doc.isNull() || !doc.isObject()) {
            emit logMessage("ERROR: Failed to parse metadata JSON.");
            emit logMessage("Reason: " + parseError.errorString());

            QString rawData = QString::fromUtf8(processBuffer).left(200);
            emit logMessage("Raw Output Snippet: " + rawData);
        }
        else {
            QJsonObject obj = doc.object();
            QString title = obj["title"].toString();
            QString uploader = obj["uploader"].toString();
            QString thumb = obj["thumbnail"].toString();

            // --- METADATA PARSING ---
            QString duration = obj["duration_string"].toString();
            if (duration.isEmpty()) {
                int durationSeconds = obj["duration"].toInt();
                int m = durationSeconds / 60;
                int s = durationSeconds % 60;
                duration = QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
            }

            int views = obj["view_count"].toInt();
            QString viewCount = QLocale(QLocale::English).toString(views);
            if (views == 0) viewCount = "N/A";

            QString rawDate = obj["upload_date"].toString();
            QString uploadDate = rawDate;
            if (rawDate.length() == 8) {
                uploadDate = rawDate.left(4) + "-" + rawDate.mid(4, 2) + "-" + rawDate.right(2);
            }

            QString streamUrl = obj["url"].toString();

            if (streamUrl.isEmpty() && obj.contains("requested_formats")) {
                QJsonArray formats = obj["requested_formats"].toArray();
                if (!formats.isEmpty()) {
                    streamUrl = formats[0].toObject()["url"].toString();
                    emit logMessage("Note: Extracted stream URL from requested_formats fallback.");
                }
            }

            emit logMessage("Metadata successfully parsed.");
            emit metadataReady(title, uploader, duration, viewCount, uploadDate, thumb);

            if (!streamUrl.isEmpty()) {
                emit streamUrlReady(QUrl(streamUrl));
            }
            else {
                emit logMessage("Warning: Direct stream URL not found in metadata.");
            }
        }
    }
    else {
        if (exitCode != 0) {
            emit logMessage(QString("Download failed. Exit code: %1").arg(exitCode));
            emit finished(false);
        }
        else {
            emit logMessage("Download completed.");
            emit finished(true);
        }
    }

    currentMode = JobMode::Idle;
}