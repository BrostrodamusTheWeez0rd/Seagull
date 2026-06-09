#include "SgUpdater.h"
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QDateTime>

SgUpdater::SgUpdater(QObject* parent) : QObject(parent) {
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

// ---------------------------------------------------------------------------
// Tool version helpers
// ---------------------------------------------------------------------------

QString SgUpdater::localYtDlpVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
    QProcess p;
    p.start(exePath, { "--version" });
    p.waitForFinished(5000);
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

QString SgUpdater::localDenoVersion() const {
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

QString SgUpdater::localFfmpegVersion() const {
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
void SgUpdater::resolveLatestVersion(const QString& latestReleaseUrl, const QString& kind) {
    QNetworkRequest req;
    req.setUrl(QUrl(latestReleaseUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->head(req);
    reply->setProperty("downloadType", "version-check");
    reply->setProperty("versionKind", kind);
}

void SgUpdater::checkForUpdates() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    qint64 lastCheck = settings.value("Updates/LastChecked", 0).toLongLong();
    qint64 now = QDateTime::currentSecsSinceEpoch();

    if (now - lastCheck < 120) {
        emit updateStatus("Skipping update check (cooldown active).");
        return;
    }

    settings.setValue("Updates/LastChecked", now);
    settings.sync();

    emit updateStatus("Checking yt-dlp version...");
    resolveLatestVersion("https://github.com/yt-dlp/yt-dlp/releases/latest", "yt-dlp");
}

void SgUpdater::checkForDenoUpdate() {
    emit updateStatus("Checking Deno version...");
    resolveLatestVersion("https://github.com/denoland/deno/releases/latest", "deno");
}

void SgUpdater::checkForFfmpegUpdate() {
    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    bool hasFfmpeg = QFile::exists(toolsDir + "/ffmpeg.exe");
    bool hasFfprobe = QFile::exists(toolsDir + "/ffprobe.exe");

    emit updateStatus("Checking ffmpeg version...");

    // gyan.dev publishes the current release version as plain text
    QString latest = fetchRemoteText("https://www.gyan.dev/ffmpeg/builds/release-version").trimmed();

    if (hasFfmpeg && hasFfprobe) {
        QString local = localFfmpegVersion(); // e.g. "8.1.1-essentials_build-www.gyan.dev"
        if (latest.isEmpty()) {
            emit updateStatus("ffmpeg present (" + local + "), could not check latest — keeping.");
            return;
        }
        // Local version string begins with the published version (e.g. "8.1.1-...")
        if (local.startsWith(latest)) {
            emit updateStatus("ffmpeg up to date (" + latest + ").");
            return;
        }
        emit updateStatus("ffmpeg outdated (" + local + " -> " + latest + ") — downloading...");
    }
    else {
        emit updateStatus(latest.isEmpty()
            ? "ffmpeg not found — downloading latest..."
            : "ffmpeg not found — downloading " + latest + "...");
    }

    QDir().mkpath(toolsDir);
    QString zipPath = toolsDir + "/ffmpeg_update.zip";

    // Open the file on disk now and stream bytes into it as they arrive.
    // This avoids buffering a 100MB+ zip entirely in memory.
    QFile* zipFile = new QFile(zipPath, this);
    if (!zipFile->open(QIODevice::WriteOnly)) {
        emit updateStatus("Failed to open ffmpeg zip for writing.");
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

    connect(reply, &QNetworkReply::downloadProgress, this, &SgUpdater::onDownloadProgress);

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
            emit updateStatus("ffmpeg download failed: " + reply->errorString());
            QFile::remove(zipPath);
            reply->deleteLater();
            return;
        }

        // The essentials zip is ~103 MB. Anything under 90 MB means a truncated
        // or redirected-to-HTML download — reject it so we never extract garbage.
        QFileInfo info(zipPath);
        if (info.size() < 90000000) {
            emit updateStatus("ffmpeg zip incomplete (" + QString::number(info.size() / 1024 / 1024) + " MB, expected ~103 MB) — aborting.");
            QFile::remove(zipPath);
            reply->deleteLater();
            return;
        }
        reply->deleteLater();

        // Verify the zip against gyan.dev's published .sha256 before extracting.
        // The .sha256 file contains "<hash> *ffmpeg-release-essentials.zip"
        emit updateStatus("Verifying ffmpeg zip hash...");
        QString shaLine = fetchRemoteText("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256");
        QString expected = shaLine.section(' ', 0, 0).trimmed();
        if (!verifyHash(zipPath, expected, "ffmpeg zip")) {
            QFile::remove(zipPath);
            return;
        }

        emit updateStatus("ffmpeg zip verified (" + QString::number(info.size() / 1024 / 1024) + " MB) — extracting...");

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
            if (!psOut.isEmpty()) emit updateStatus("Archive bin/ contents: " + psOut.replace("\n", ", "));
            if (!psErr.isEmpty()) emit updateStatus("PS error: " + psErr);
            ps->deleteLater();
            QFile::remove(zipPath);

            qint64 ffmpegSz = QFile::exists(exeDest) ? QFileInfo(exeDest).size() : 0;
            qint64 ffprobeSz = QFile::exists(probeDest) ? QFileInfo(probeDest).size() : 0;

            // The zip is already SHA256-verified, so the binaries are trustworthy.
            // Keep a low floor only to catch a totally failed extraction (0 bytes etc.).
            if (ffmpegSz < 10000000) {
                emit updateStatus("ffmpeg.exe extraction failed ("
                    + QString::number(ffmpegSz / 1024 / 1024) + " MB) — removing.");
                QFile::remove(exeDest);
                QFile::remove(probeDest);
                return;
            }

            emit updateStatus("ffmpeg installed ("
                + QString::number(ffmpegSz / 1024 / 1024) + " MB), ffprobe ("
                + QString::number(ffprobeSz / 1024 / 1024) + " MB).");
            });
        ps->start("powershell", { "-NoProfile", "-NonInteractive", "-Command", psCmd });
        });
}

void SgUpdater::onReleaseInfoReceived(QNetworkReply* reply) {
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
        emit updateStatus(kind + ": could not resolve latest version, skipping.");
        if (kind == "yt-dlp") checkForDenoUpdate();
        else if (kind == "deno") checkForFfmpegUpdate();
        return;
    }

    if (kind == "yt-dlp") {
        QString local = localYtDlpVersion();
        if (!local.isEmpty() && local >= latestVersion) {
            emit updateStatus("yt-dlp up to date (" + local + ").");
            checkForDenoUpdate();
            return;
        }
        emit updateStatus(local.isEmpty()
            ? "yt-dlp missing — downloading " + latestVersion + "..."
            : "yt-dlp outdated (" + local + " -> " + latestVersion + ") — downloading...");
        downloadNewExe("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe");
    }
    else if (kind == "deno") {
        QString local = localDenoVersion(); // returns like "v2.2.0"
        if (!local.isEmpty() && local >= latestVersion) {
            emit updateStatus("Deno up to date (" + local + ").");
            checkForFfmpegUpdate();
            return;
        }
        emit updateStatus(local.isEmpty()
            ? "Deno missing — downloading " + latestVersion + "..."
            : "Deno outdated (" + local + " -> " + latestVersion + ") — downloading...");
        downloadNewDeno("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip");
    }
}

void SgUpdater::downloadNewExe(const QString& exeUrl) {
    QNetworkRequest req;
    req.setUrl(QUrl(exeUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("downloadType", "yt-dlp-exe");
    connect(reply, &QNetworkReply::downloadProgress, this, &SgUpdater::onDownloadProgress);
}

void SgUpdater::downloadNewDeno(const QString& zipUrl) {
    QNetworkRequest req;
    req.setUrl(QUrl(zipUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    QNetworkReply* reply = m_nam->get(req);
    reply->setProperty("downloadType", "deno-zip");
    connect(reply, &QNetworkReply::downloadProgress, this, &SgUpdater::onDownloadProgress);
}

void SgUpdater::onDownloadProgress(qint64 received, qint64 total) {
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
        emit updateStatus(prefix + ": " + QString::number(pct) + "%");
    }
}

QString SgUpdater::computeFileSha256(const QString& filePath) const {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // addData(QIODevice*) streams the file in chunks — safe for 150MB files
    if (!hash.addData(&f)) return QString();
    f.close();
    return QString::fromLatin1(hash.result().toHex());
}

QString SgUpdater::fetchRemoteText(const QString& url) const {
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
bool SgUpdater::verifyHash(const QString& filePath, const QString& expectedHash, const QString& label) {
    if (expectedHash.isEmpty()) {
        emit updateStatus(label + ": no published hash available — skipping verification.");
        return true; // don't block install if the hash file couldn't be fetched
    }
    QString actual = computeFileSha256(filePath);
    if (actual.isEmpty()) {
        emit updateStatus(label + ": could not compute local hash.");
        return false;
    }
    if (actual.compare(expectedHash, Qt::CaseInsensitive) == 0) {
        emit updateStatus(label + ": hash verified OK.");
        return true;
    }
    emit updateStatus(label + ": HASH MISMATCH — rejecting file.");
    emit updateStatus("  expected: " + expectedHash.left(16) + "...");
    emit updateStatus("  actual:   " + actual.left(16) + "...");
    return false;
}

bool SgUpdater::extractDenoZip(const QString& zipPath, const QString& targetDir) {
    QProcess process;
    QStringList args;
    args << "-NoProfile" << "-Command"
        << QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force").arg(zipPath, targetDir);

    process.start("powershell.exe", args);
    return process.waitForFinished(30000) && (process.exitCode() == 0);
}

void SgUpdater::onExeDownloadFinished(QNetworkReply* reply) {
    QString downloadType = reply->property("downloadType").toString();
    QByteArray fileData = reply->readAll();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit updateStatus("Download failed: " + reply->errorString());
        return;
    }

    if (fileData.size() < 5000000) {
        emit updateStatus("Download too small — aborting update.");
        return;
    }

    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    QDir().mkpath(toolsDir);

    if (downloadType == "yt-dlp-exe") {
        QString exePath = toolsDir + "/yt-dlp.exe";
        QString tmpPath = toolsDir + "/yt-dlp_new.exe";

        QFile tmp(tmpPath);
        if (!tmp.open(QIODevice::WriteOnly)) {
            emit updateStatus("Failed to write yt-dlp to disk.");
            return;
        }
        tmp.write(fileData);
        tmp.close();

        // Verify against the release SHA2-256SUMS (lines are "hash  filename")
        emit updateStatus("Verifying yt-dlp hash...");
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
            emit updateStatus("yt-dlp updated successfully.");
        }
        else {
            QFile::rename(exePath + ".bak", exePath);
            emit updateStatus("yt-dlp update failed (file in use?).");
        }

        // Chain to Deno
        checkForDenoUpdate();
    }
    else if (downloadType == "deno-zip") {
        QString zipPath = toolsDir + "/deno_temp.zip";
        QString exePath = toolsDir + "/deno.exe";

        QFile file(zipPath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit updateStatus("Failed to write Deno zip to disk.");
            return;
        }
        file.write(fileData);
        file.close();

        // Verify zip against Deno's published .sha256sum (format: "hash  filename")
        emit updateStatus("Verifying Deno hash...");
        QString sumLine = fetchRemoteText("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum");
        QString expected = sumLine.section(' ', 0, 0).trimmed();
        if (!verifyHash(zipPath, expected, "Deno")) {
            QFile::remove(zipPath);
            checkForFfmpegUpdate();
            return;
        }

        emit updateStatus("Extracting deno.exe...");

        QFile::remove(exePath + ".bak");
        if (QFile::exists(exePath))
            QFile::rename(exePath, exePath + ".bak");

        if (extractDenoZip(zipPath, toolsDir)) {
            emit updateStatus("Deno updated successfully.");
            QFile::remove(zipPath);
            QFile::remove(exePath + ".bak");
        }
        else {
            emit updateStatus("Deno extraction failed — rolling back.");
            QFile::remove(zipPath);
            if (QFile::exists(exePath + ".bak"))
                QFile::rename(exePath + ".bak", exePath);
        }

        // Chain to ffmpeg regardless of outcome
        checkForFfmpegUpdate();
    }
}
