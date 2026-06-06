#include "SgYtDlp.h"
#include <QCoreApplication>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QRegularExpression>
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

    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
        QString type = reply->property("downloadType").toString();
        if (type == "yt-dlp-exe" || type == "deno-zip")
            onExeDownloadFinished(reply);
        else
            onReleaseInfoReceived(reply);
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

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        emit logMessage("Operation failed (yt-dlp). Code: " + QString::number(exitCode));
        emit finished(false);
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

// ---------------------------------------------------------------------------
// Dependency Update Checking (yt-dlp & Deno JavaScript Runtime)
// ---------------------------------------------------------------------------

QString SgYtDlp::localYtDlpVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
    QProcess p;
    p.start(exePath, { "--version" });
    p.waitForFinished(5000);
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

QString SgYtDlp::localDenoVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/deno.exe";
    if (!QFile::exists(exePath)) return QString();
    QProcess p;
    p.start(exePath, { "--version" });
    p.waitForFinished(5000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    QRegularExpression rx("deno\\s+([0-9.]+)");
    QRegularExpressionMatch match = rx.match(out);
    if (match.hasMatch()) return "v" + match.captured(1);
    return QString();
}

void SgYtDlp::checkForYtDlpUpdate() {
    emit ytDlpUpdateStatus("Checking for updates...");
    QUrl apiUrl("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest");
    QNetworkRequest req;
    req.setUrl(apiUrl);
    req.setRawHeader("User-Agent", "Seagull-Player");
    m_nam->get(req);
}

void SgYtDlp::onReleaseInfoReceived(QNetworkReply* reply) {
    QUrl url = reply->url();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        emit ytDlpUpdateStatus("Update check failed: " + reply->errorString());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QString latestTag = doc.object()["tag_name"].toString();

    if (url.toString().contains("yt-dlp/yt-dlp")) {
        QString localVersion = localYtDlpVersion();
        emit ytDlpUpdateStatus("yt-dlp local: " + localVersion + " | latest: " + latestTag);

        bool needsUpdate = localVersion.isEmpty() || latestTag.isEmpty() || (latestTag > localVersion);

        if (!needsUpdate) {
            emit ytDlpUpdateStatus("yt-dlp is up to date. Verifying Deno engine environment...");
            QUrl denoApiUrl("https://api.github.com/repos/denoland/deno/releases/latest");
            QNetworkRequest req(denoApiUrl);
            req.setRawHeader("User-Agent", "Seagull-Player");
            m_nam->get(req);
            return;
        }

        emit ytDlpUpdateStatus("New yt-dlp version detected: " + latestTag + " - downloading...");
        QJsonArray assets = doc.object()["assets"].toArray();
        QString exeUrl;
        for (const auto& a : assets) {
            QJsonObject asset = a.toObject();
            if (asset["name"].toString() == "yt-dlp.exe") {
                exeUrl = asset["browser_download_url"].toString();
                break;
            }
        }

        if (exeUrl.isEmpty()) {
            emit ytDlpUpdateStatus("Could not locate yt-dlp.exe asset link.");
            return;
        }
        downloadNewExe(exeUrl);
    }
    else if (url.toString().contains("denoland/deno")) {
        QString localVersion = localDenoVersion();
        if (localVersion.isEmpty()) {
            emit ytDlpUpdateStatus("Deno JS engine not found. Unpacking companion binary to tools...");
        }
        else {
            emit ytDlpUpdateStatus("Deno local: " + localVersion + " | latest: " + latestTag);
        }

        if (!localVersion.isEmpty() && latestTag <= localVersion) {
            emit ytDlpUpdateStatus("Deno JS environment is up to date.");
            return;
        }

        emit ytDlpUpdateStatus("New Deno version detected: " + latestTag + " - downloading payload archive...");
        QJsonArray assets = doc.object()["assets"].toArray();
        QString zipUrl;
        for (const auto& a : assets) {
            QJsonObject asset = a.toObject();
            if (asset["name"].toString() == "deno-x86_64-pc-windows-msvc.zip") {
                zipUrl = asset["browser_download_url"].toString();
                break;
            }
        }

        if (zipUrl.isEmpty()) {
            emit ytDlpUpdateStatus("Failed to resolve compatible Deno runtime bundle target.");
            return;
        }
        downloadNewDeno(zipUrl);
    }
}

void SgYtDlp::downloadNewExe(const QString& exeUrl) {
    QNetworkRequest req;
    req.setUrl(QUrl(exeUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("downloadType", "yt-dlp-exe");
    connect(reply, &QNetworkReply::downloadProgress, this, &SgYtDlp::onDownloadProgress);
}

void SgYtDlp::downloadNewDeno(const QString& zipUrl) {
    QNetworkRequest req;
    req.setUrl(QUrl(zipUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("downloadType", "deno-zip");
    connect(reply, &QNetworkReply::downloadProgress, this, &SgYtDlp::onDownloadProgress);
}

void SgYtDlp::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
        QString prefix = "Processing asset package";
        if (reply) {
            QString type = reply->property("downloadType").toString();
            if (type == "yt-dlp-exe") prefix = "Downloading yt-dlp build";
            if (type == "deno-zip") prefix = "Downloading Deno runtime package";
        }
        int pct = static_cast<int>(received * 100 / total);
        emit ytDlpUpdateStatus(prefix + ": " + QString::number(pct) + "%");
    }
}

bool SgYtDlp::extractDenoZip(const QString& zipPath, const QString& targetDir) {
    QProcess process;
    QStringList args;
    args << "-NoProfile" << "-Command"
        << QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force").arg(zipPath, targetDir);

    process.start("powershell.exe", args);
    return process.waitForFinished(30000) && (process.exitCode() == 0);
}

void SgYtDlp::onExeDownloadFinished(QNetworkReply* reply) {
    QString downloadType = reply->property("downloadType").toString();
    QByteArray fileData = reply->readAll();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit ytDlpUpdateStatus("Asset download failed: " + reply->errorString());
        return;
    }

    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    QDir().mkpath(toolsDir);

    if (downloadType == "yt-dlp-exe") {
        QString exePath = toolsDir + "/yt-dlp.exe";
        QString tmpPath = toolsDir + "/yt-dlp_new.exe";

        QFile tmp(tmpPath);
        if (!tmp.open(QIODevice::WriteOnly)) {
            emit ytDlpUpdateStatus("Failed to open staging location for streaming data.");
            return;
        }
        tmp.write(fileData);
        tmp.close();

        QFile::remove(exePath + ".bak");
        QFile::rename(exePath, exePath + ".bak");
        if (QFile::rename(tmpPath, exePath)) {
            emit ytDlpUpdateStatus("yt-dlp binary updated successfully.");
        }
        else {
            QFile::rename(exePath + ".bak", exePath);
            emit ytDlpUpdateStatus("File update failed (permissions error). Preserving old runtime snapshot.");
        }

        // Forward execution straight to verifying runtime engines
        QUrl denoApiUrl("https://api.github.com/repos/denoland/deno/releases/latest");
        QNetworkRequest req(denoApiUrl);
        req.setRawHeader("User-Agent", "Seagull-Player");
        m_nam->get(req);
    }
    else if (downloadType == "deno-zip") {
        QString zipPath = toolsDir + "/deno_temp.zip";
        QString exePath = toolsDir + "/deno.exe";

        QFile file(zipPath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit ytDlpUpdateStatus("Failed to save runtime archive bundle.");
            return;
        }
        file.write(fileData);
        file.close();

        emit ytDlpUpdateStatus("Decompressing memory buffer and isolating executable target...");

        QFile::remove(exePath + ".bak");
        if (QFile::exists(exePath)) {
            QFile::rename(exePath, exePath + ".bak");
        }

        if (extractDenoZip(zipPath, toolsDir)) {
            emit ytDlpUpdateStatus("Deno JavaScript engine successfully unpacked to local companion toolchain directory.");
            QFile::remove(zipPath);
            QFile::remove(exePath + ".bak");
        }
        else {
            emit ytDlpUpdateStatus("Unpacking runtime executable failed. Rolling back local changes.");
            QFile::remove(zipPath);
            if (QFile::exists(exePath + ".bak")) {
                QFile::rename(exePath + ".bak", exePath);
            }
        }
    }
}