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
#include <QDateTime>
#include <QCryptographicHash>
#include <QEventLoop>

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
        else if (type == "ffmpeg-zip-stream")
        {
        } // handled by its own finished lambda in checkForFfmpegUpdate
        else if (type == "version-check")
            onReleaseInfoReceived(reply);
        else
            reply->deleteLater();
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

    QString chosenFormat;
    if (!formatId.isEmpty())
        chosenFormat = QString("%1+bestaudio/best").arg(formatId);  // user picked from quality menu
    else
        chosenFormat = defaultStreamFormat();                        // honor default Stream Quality setting
    args << "-f" << chosenFormat;

    args << url;

    emit logMessage("Fetching metadata for: " + url + "  [format: " + chosenFormat + "]");
    m_process->start(exePath, args);
}

// Translates the "Streaming/Quality" config label into a yt-dlp format string.
// "Best Available" (or unset) -> bestvideo+bestaudio/best
// "1080p" / "2160p (4K)" etc.  -> height-capped selection with graceful fallback
QString SgYtDlp::defaultStreamFormat() const {
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    QString quality = settings.value("Streaming/Quality", "Best Available").toString();

    if (quality.isEmpty() || quality == "Best Available")
        return "bestvideo+bestaudio/best";

    QRegularExpression re("(\\d+)");
    QRegularExpressionMatch m = re.match(quality);
    if (!m.hasMatch())
        return "bestvideo+bestaudio/best";

    QString height = m.captured(1);
    return QString("bestvideo[height<=%1]+bestaudio/best[height<=%1]/best").arg(height);
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
// Tool version helpers
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
    return match.hasMatch() ? "v" + match.captured(1) : QString();
}

QString SgYtDlp::localFfmpegVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/ffmpeg.exe";
    if (!QFile::exists(exePath)) return QString();
    QProcess p;
    p.start(exePath, { "-version" });
    p.waitForFinished(5000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    QRegularExpression re("ffmpeg version ([^\\s]+)");
    auto m = re.match(out);
    return m.hasMatch() ? m.captured(1) : QString();
}

// ---------------------------------------------------------------------------
// Update chain: yt-dlp -> Deno -> ffmpeg
// All use permanent direct URLs — no GitHub API needed.
// ---------------------------------------------------------------------------

// Helper: resolve the latest version tag by following the GitHub "latest" redirect.
// GitHub redirects /releases/latest to /releases/tag/<VERSION>, so the final URL
// contains the version without downloading the asset.
void SgYtDlp::resolveLatestVersion(const QString& latestReleaseUrl, const QString& kind) {
    QNetworkRequest req;
    req.setUrl(QUrl(latestReleaseUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->head(req);
    reply->setProperty("downloadType", "version-check");
    reply->setProperty("versionKind", kind);
}

void SgYtDlp::checkForYtDlpUpdate() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    qint64 lastCheck = settings.value("Updates/LastChecked", 0).toLongLong();
    qint64 now = QDateTime::currentSecsSinceEpoch();

    if (now - lastCheck < 120) {
        emit logMessage("Skipping update check (cooldown active).");
        return;
    }

    settings.setValue("Updates/LastChecked", now);
    settings.sync();

    emit ytDlpUpdateStatus("Checking yt-dlp version...");
    resolveLatestVersion("https://github.com/yt-dlp/yt-dlp/releases/latest", "yt-dlp");
}

void SgYtDlp::checkForDenoUpdate() {
    emit ytDlpUpdateStatus("Checking Deno version...");
    resolveLatestVersion("https://github.com/denoland/deno/releases/latest", "deno");
}

void SgYtDlp::checkForFfmpegUpdate() {
    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    bool hasFfmpeg = QFile::exists(toolsDir + "/ffmpeg.exe");
    bool hasFfprobe = QFile::exists(toolsDir + "/ffprobe.exe");

    emit ytDlpUpdateStatus("Checking ffmpeg version...");

    // gyan.dev publishes the current release version as plain text
    QString latest = fetchRemoteText("https://www.gyan.dev/ffmpeg/builds/release-version").trimmed();

    if (hasFfmpeg && hasFfprobe) {
        QString local = localFfmpegVersion(); // e.g. "8.1.1-essentials_build-www.gyan.dev"
        if (latest.isEmpty()) {
            emit ytDlpUpdateStatus("ffmpeg present (" + local + "), could not check latest — keeping.");
            return;
        }
        // Local version string begins with the published version (e.g. "8.1.1-...")
        if (local.startsWith(latest)) {
            emit ytDlpUpdateStatus("ffmpeg up to date (" + latest + ").");
            return;
        }
        emit ytDlpUpdateStatus("ffmpeg outdated (" + local + " -> " + latest + ") — downloading...");
    }
    else {
        emit ytDlpUpdateStatus(latest.isEmpty()
            ? "ffmpeg not found — downloading latest..."
            : "ffmpeg not found — downloading " + latest + "...");
    }

    QDir().mkpath(toolsDir);
    QString zipPath = toolsDir + "/ffmpeg_update.zip";

    // Open the file on disk now and stream bytes into it as they arrive.
    // This avoids buffering a 100MB+ zip entirely in memory.
    QFile* zipFile = new QFile(zipPath, this);
    if (!zipFile->open(QIODevice::WriteOnly)) {
        emit ytDlpUpdateStatus("Failed to open ffmpeg zip for writing.");
        delete zipFile;
        return;
    }

    QNetworkRequest req;
    req.setUrl(QUrl("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"));
    req.setRawHeader("User-Agent", "Seagull-Player");
    // gyan.dev redirects to a versioned file on GitHub — must follow redirects
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("downloadType", "ffmpeg-zip-stream");
    reply->setProperty("ffmpegZipPath", zipPath);

    // Stream each chunk straight to disk
    connect(reply, &QNetworkReply::readyRead, this, [reply, zipFile]() {
        zipFile->write(reply->readAll());
        });

    connect(reply, &QNetworkReply::downloadProgress, this, &SgYtDlp::onDownloadProgress);

    // When done, flush any remaining buffered bytes then close and extract
    connect(reply, &QNetworkReply::finished, this, [this, reply, zipFile, zipPath]() {
        // readyRead doesn't always deliver the final bytes before finished fires;
        // do one last read to catch any remainder still buffered in the reply.
        QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty())
            zipFile->write(remaining);
        zipFile->flush();
        zipFile->close();
        zipFile->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit ytDlpUpdateStatus("ffmpeg download failed: " + reply->errorString());
            QFile::remove(zipPath);
            reply->deleteLater();
            return;
        }

        // The essentials zip is ~103 MB. Anything under 90 MB means a truncated
        // or redirected-to-HTML download — reject it so we never extract garbage.
        QFileInfo info(zipPath);
        if (info.size() < 90000000) {
            emit ytDlpUpdateStatus("ffmpeg zip incomplete (" + QString::number(info.size() / 1024 / 1024) + " MB, expected ~103 MB) — aborting.");
            QFile::remove(zipPath);
            reply->deleteLater();
            return;
        }
        reply->deleteLater();

        // Verify the zip against gyan.dev's published .sha256 before extracting.
        // The .sha256 file contains "<hash> *ffmpeg-release-essentials.zip"
        emit ytDlpUpdateStatus("Verifying ffmpeg zip hash...");
        QString shaLine = fetchRemoteText("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256");
        QString expected = shaLine.section(' ', 0, 0).trimmed();
        if (!verifyHash(zipPath, expected, "ffmpeg zip")) {
            QFile::remove(zipPath);
            return;
        }

        emit ytDlpUpdateStatus("ffmpeg zip verified (" + QString::number(info.size() / 1024 / 1024) + " MB) — extracting...");

        QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
        QString exeDest = toolsDir + "/ffmpeg.exe";
        QString tempDir = toolsDir + "/ffmpeg_extract_temp";

        QDir().mkpath(tempDir);

        // Use tar.exe (bsdtar) to extract — it handles the gyan zip reliably and
        // copies the two binaries straight out of the archive's bin/ folder.
        QString probeDest = toolsDir + "/ffprobe.exe";

        // Pull only the two files we need directly from the archive using their
        // in-archive path (subfolder is dynamic, so wildcard the leading folder).
        QString psCmd = QString(
            "$tmp = '%1'; $dstDir = '%2'; $zip = '%3'; "
            "& tar.exe -xf $zip -C $tmp; "
            "$sub = (Get-ChildItem -Path $tmp -Directory | Select-Object -First 1).FullName; "
            "$binDir = Join-Path $sub 'bin'; "
            // Diagnostic: report what's actually in the bin folder and their sizes
            "Get-ChildItem -Path $binDir -Filter *.exe | ForEach-Object { Write-Output (\"$($_.Name) = $([math]::Round($_.Length/1MB)) MB\") }; "
            "$ffmpeg  = Join-Path $binDir 'ffmpeg.exe'; "
            "$ffprobe = Join-Path $binDir 'ffprobe.exe'; "
            "if ((Test-Path $ffmpeg) -and (Test-Path $ffprobe)) { "
            "  Copy-Item $ffmpeg  -Destination (Join-Path $dstDir 'ffmpeg.exe')  -Force; "
            "  Copy-Item $ffprobe -Destination (Join-Path $dstDir 'ffprobe.exe') -Force "
            "} else { Write-Error 'binaries not found in archive bin folder' }; "
            "Remove-Item -Recurse -Force $tmp"
        ).arg(tempDir, toolsDir, zipPath);

        QProcess* ps = new QProcess(this);
        connect(ps, &QProcess::finished, this, [this, ps, zipPath, exeDest, probeDest](int code, QProcess::ExitStatus) {
            QString psOut = QString::fromLocal8Bit(ps->readAllStandardOutput()).trimmed();
            QString psErr = QString::fromLocal8Bit(ps->readAllStandardError()).trimmed();
            if (!psOut.isEmpty()) emit ytDlpUpdateStatus("Archive bin/ contents: " + psOut.replace("\n", ", "));
            if (!psErr.isEmpty()) emit ytDlpUpdateStatus("PS error: " + psErr);
            ps->deleteLater();
            QFile::remove(zipPath);

            qint64 ffmpegSz = QFile::exists(exeDest) ? QFileInfo(exeDest).size() : 0;
            qint64 ffprobeSz = QFile::exists(probeDest) ? QFileInfo(probeDest).size() : 0;

            // The zip is already SHA256-verified, so the binaries are trustworthy.
            // Keep a low floor only to catch a totally failed extraction (0 bytes etc.).
            if (ffmpegSz < 10000000) {
                emit ytDlpUpdateStatus("ffmpeg.exe extraction failed ("
                    + QString::number(ffmpegSz / 1024 / 1024) + " MB) — removing.");
                QFile::remove(exeDest);
                QFile::remove(probeDest);
                return;
            }

            emit ytDlpUpdateStatus("ffmpeg installed ("
                + QString::number(ffmpegSz / 1024 / 1024) + " MB), ffprobe ("
                + QString::number(ffprobeSz / 1024 / 1024) + " MB).");
            });
        ps->start("powershell", { "-NoProfile", "-NonInteractive", "-Command", psCmd });
        });
}

void SgYtDlp::onReleaseInfoReceived(QNetworkReply* reply) {
    QString kind = reply->property("versionKind").toString();

    // The redirect target URL ends in /releases/tag/<VERSION>
    QUrl finalUrl = reply->url();
    QVariant redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirectTarget.isValid()) {
        finalUrl = reply->url().resolved(redirectTarget.toUrl());
    }
    reply->deleteLater();

    QString latestVersion = finalUrl.toString().section('/', -1).trimmed();
    // Deno tags are like "v2.2.0"; yt-dlp tags are like "2026.01.15"
    if (latestVersion.isEmpty() || latestVersion == "latest") {
        emit ytDlpUpdateStatus(kind + ": could not resolve latest version, skipping.");
        if (kind == "yt-dlp") checkForDenoUpdate();
        else if (kind == "deno") checkForFfmpegUpdate();
        return;
    }

    if (kind == "yt-dlp") {
        QString local = localYtDlpVersion();
        if (!local.isEmpty() && local >= latestVersion) {
            emit ytDlpUpdateStatus("yt-dlp up to date (" + local + ").");
            checkForDenoUpdate();
            return;
        }
        emit ytDlpUpdateStatus(local.isEmpty()
            ? "yt-dlp missing — downloading " + latestVersion + "..."
            : "yt-dlp outdated (" + local + " -> " + latestVersion + ") — downloading...");
        downloadNewExe("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe");
    }
    else if (kind == "deno") {
        QString local = localDenoVersion(); // returns like "v2.2.0"
        if (!local.isEmpty() && local >= latestVersion) {
            emit ytDlpUpdateStatus("Deno up to date (" + local + ").");
            checkForFfmpegUpdate();
            return;
        }
        emit ytDlpUpdateStatus(local.isEmpty()
            ? "Deno missing — downloading " + latestVersion + "..."
            : "Deno outdated (" + local + " -> " + latestVersion + ") — downloading...");
        downloadNewDeno("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip");
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
        QString prefix = "Downloading";
        if (reply) {
            QString type = reply->property("downloadType").toString();
            if (type == "yt-dlp-exe") prefix = "Downloading yt-dlp";
            if (type == "deno-zip")   prefix = "Downloading Deno";
            if (type == "ffmpeg-zip") prefix = "Downloading ffmpeg";
        }
        int pct = static_cast<int>(received * 100 / total);
        emit ytDlpUpdateStatus(prefix + ": " + QString::number(pct) + "%");
    }
}

QString SgYtDlp::computeFileSha256(const QString& filePath) const {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // addData(QIODevice*) streams the file in chunks — safe for 150MB files
    if (!hash.addData(&f)) return QString();
    f.close();
    return QString::fromLatin1(hash.result().toHex());
}

QString SgYtDlp::fetchRemoteText(const QString& url) const {
    QNetworkAccessManager nam;
    QNetworkRequest req;
    req.setUrl(QUrl(url));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QString result;
    if (reply->error() == QNetworkReply::NoError)
        result = QString::fromUtf8(reply->readAll()).trimmed();
    reply->deleteLater();
    return result;
}

// Verifies a local file against an expected SHA256 (case-insensitive hex).
// Returns true if they match. Logs the outcome.
bool SgYtDlp::verifyHash(const QString& filePath, const QString& expectedHash, const QString& label) {
    if (expectedHash.isEmpty()) {
        emit ytDlpUpdateStatus(label + ": no published hash available — skipping verification.");
        return true; // don't block install if the hash file couldn't be fetched
    }
    QString actual = computeFileSha256(filePath);
    if (actual.isEmpty()) {
        emit ytDlpUpdateStatus(label + ": could not compute local hash.");
        return false;
    }
    if (actual.compare(expectedHash, Qt::CaseInsensitive) == 0) {
        emit ytDlpUpdateStatus(label + ": hash verified OK.");
        return true;
    }
    emit ytDlpUpdateStatus(label + ": HASH MISMATCH — rejecting file.");
    emit ytDlpUpdateStatus("  expected: " + expectedHash.left(16) + "...");
    emit ytDlpUpdateStatus("  actual:   " + actual.left(16) + "...");
    return false;
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
        emit ytDlpUpdateStatus("Download failed: " + reply->errorString());
        return;
    }

    if (fileData.size() < 5000000) {
        emit ytDlpUpdateStatus("Download too small — aborting update.");
        return;
    }

    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    QDir().mkpath(toolsDir);

    if (downloadType == "yt-dlp-exe") {
        QString exePath = toolsDir + "/yt-dlp.exe";
        QString tmpPath = toolsDir + "/yt-dlp_new.exe";

        QFile tmp(tmpPath);
        if (!tmp.open(QIODevice::WriteOnly)) {
            emit ytDlpUpdateStatus("Failed to write yt-dlp to disk.");
            return;
        }
        tmp.write(fileData);
        tmp.close();

        // Verify against the release SHA2-256SUMS (lines are "hash  filename")
        emit ytDlpUpdateStatus("Verifying yt-dlp hash...");
        QString sums = fetchRemoteText("https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS");
        QString expected;
        for (const QString& line : sums.split('\n')) {
            QString trimmed = line.trimmed();
            if (trimmed.endsWith("yt-dlp.exe")) {
                expected = trimmed.section(' ', 0, 0).trimmed();
                break;
            }
        }

        if (!verifyHash(tmpPath, expected, "yt-dlp")) {
            QFile::remove(tmpPath);
            checkForDenoUpdate();
            return;
        }

        QFile::remove(exePath + ".bak");
        QFile::rename(exePath, exePath + ".bak");
        if (QFile::rename(tmpPath, exePath)) {
            QFile::remove(exePath + ".bak");  // clean up bak on success
            emit ytDlpUpdateStatus("yt-dlp updated successfully.");
        }
        else {
            QFile::rename(exePath + ".bak", exePath);
            emit ytDlpUpdateStatus("yt-dlp update failed (file in use?).");
        }

        // Chain to Deno
        checkForDenoUpdate();
    }
    else if (downloadType == "deno-zip") {
        QString zipPath = toolsDir + "/deno_temp.zip";
        QString exePath = toolsDir + "/deno.exe";

        QFile file(zipPath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit ytDlpUpdateStatus("Failed to write Deno zip to disk.");
            return;
        }
        file.write(fileData);
        file.close();

        // Verify zip against Deno's published .sha256sum (format: "hash  filename")
        emit ytDlpUpdateStatus("Verifying Deno hash...");
        QString sumLine = fetchRemoteText("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum");
        QString expected = sumLine.section(' ', 0, 0).trimmed();
        if (!verifyHash(zipPath, expected, "Deno")) {
            QFile::remove(zipPath);
            checkForFfmpegUpdate();
            return;
        }

        emit ytDlpUpdateStatus("Extracting deno.exe...");

        QFile::remove(exePath + ".bak");
        if (QFile::exists(exePath))
            QFile::rename(exePath, exePath + ".bak");

        if (extractDenoZip(zipPath, toolsDir)) {
            emit ytDlpUpdateStatus("Deno updated successfully.");
            QFile::remove(zipPath);
            QFile::remove(exePath + ".bak");
        }
        else {
            emit ytDlpUpdateStatus("Deno extraction failed — rolling back.");
            QFile::remove(zipPath);
            if (QFile::exists(exePath + ".bak"))
                QFile::rename(exePath + ".bak", exePath);
        }

        // Chain to ffmpeg regardless of outcome
        checkForFfmpegUpdate();
    }

}