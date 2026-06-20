#include "SgUpdater.h"
#include "SgPaths.h"
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
        } // handled by its own finished lambda in downloadFfmpeg
        else
            reply->deleteLater();
        });
}

// ---------------------------------------------------------------------------
// Tool version helpers
// ---------------------------------------------------------------------------

// Probe timeouts are generous: a freshly installed exe's first run can take
// many seconds (yt-dlp self-extracts, and the AV scans the new binary).
QString SgUpdater::localYtDlpVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
    if (!QFile::exists(exePath)) return QString();
    QProcess p;
    p.start(exePath, { "--version" });
    p.waitForFinished(20000);
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

QString SgUpdater::localDenoVersion() const {
    QString exePath = QCoreApplication::applicationDirPath() + "/tools/deno.exe";
    if (!QFile::exists(exePath)) return QString();
    QProcess p;
    p.start(exePath, { "--version" });
    p.waitForFinished(20000);
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
    p.waitForFinished(20000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    QRegularExpression re("ffmpeg version ([^\\s]+)");
    auto m = re.match(out);
    return m.hasMatch() ? m.captured(1) : QString();
}

// ---------------------------------------------------------------------------
// Phase 1 — check. Blocking, sequential, no installs.
// ---------------------------------------------------------------------------

// Resolve the latest version tag by following the GitHub "latest" redirect:
// /releases/latest lands on /releases/tag/<VERSION>, so the final URL carries
// the version without downloading anything. Empty string = couldn't resolve.
QString SgUpdater::resolveLatestTag(const QString& latestReleaseUrl) const {
    QNetworkAccessManager nam; // local: keeps these replies out of m_nam's dispatcher
    QNetworkRequest req;
    req.setUrl(QUrl(latestReleaseUrl));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam.head(req);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QString tag;
    if (reply->error() == QNetworkReply::NoError)
        tag = reply->url().toString().section('/', -1).trimmed(); // redirects already followed
    reply->deleteLater();
    return (tag == "latest") ? QString() : tag;
}

void SgUpdater::checkForUpdates(bool ignoreCooldown) {
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    qint64 lastCheck = settings.value("Updates/LastChecked", 0).toLongLong();
    qint64 now = QDateTime::currentSecsSinceEpoch();

    if (!ignoreCooldown && now - lastCheck < 120) {
        emit updateStatus("Skipping update check (cooldown active).");
        emit checkFinished({});
        return;
    }

    settings.setValue("Updates/LastChecked", now);
    settings.sync();

    m_applyQueue.clear();
    QStringList pending;

    const QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";

    // yt-dlp — date-style tags ("2026.01.15"), plain string compare works.
    emit updateStatus("Checking yt-dlp version...");
    {
        const bool present = QFile::exists(toolsDir + "/yt-dlp.exe");
        const QString latest = resolveLatestTag("https://github.com/yt-dlp/yt-dlp/releases/latest");
        const QString local = localYtDlpVersion();
        if (latest.isEmpty()) {
            emit updateStatus("yt-dlp: could not resolve latest version, skipping.");
        } else if (present && local.isEmpty()) {
            // The exe is there but --version produced nothing (often a freshly
            // installed exe still being AV-scanned). Never reinstall over it.
            emit updateStatus("yt-dlp present, could not read version, keeping.");
        } else if (present && local >= latest) {
            emit updateStatus("yt-dlp up to date (" + local + ").");
        } else {
            pending << (!present ? "yt-dlp:  not installed  →  " + latest
                                 : "yt-dlp:  " + local + "  →  " + latest);
            m_applyQueue << "yt-dlp";
        }
    }

    // Deno — tags like "v2.2.0".
    emit updateStatus("Checking Deno version...");
    {
        const bool present = QFile::exists(toolsDir + "/deno.exe");
        const QString latest = resolveLatestTag("https://github.com/denoland/deno/releases/latest");
        const QString local = localDenoVersion();
        if (latest.isEmpty()) {
            emit updateStatus("Deno: could not resolve latest version, skipping.");
        } else if (present && local.isEmpty()) {
            emit updateStatus("Deno present, could not read version, keeping.");
        } else if (present && local >= latest) {
            emit updateStatus("Deno up to date (" + local + ").");
        } else {
            pending << (!present ? "Deno:  not installed  →  " + latest
                                 : "Deno:  " + local + "  →  " + latest);
            m_applyQueue << "deno";
        }
    }

    // ffmpeg — gyan.dev publishes the current release version as plain text;
    // the local version string begins with it (e.g. "8.1.1-essentials_build-...").
    emit updateStatus("Checking ffmpeg version...");
    {
        const bool present = QFile::exists(toolsDir + "/ffmpeg.exe")
                          && QFile::exists(toolsDir + "/ffprobe.exe");
        const QString latest = fetchRemoteText("https://www.gyan.dev/ffmpeg/builds/release-version").trimmed();
        const QString local = localFfmpegVersion();
        if (present && latest.isEmpty()) {
            emit updateStatus("ffmpeg present (" + local + "), could not check latest, keeping.");
        } else if (present && local.isEmpty()) {
            emit updateStatus("ffmpeg present, could not read version, keeping.");
        } else if (present && local.startsWith(latest)) {
            emit updateStatus("ffmpeg up to date (" + latest + ").");
        } else {
            pending << (!present ? "ffmpeg:  not installed  →  " + (latest.isEmpty() ? QStringLiteral("latest") : latest)
                                 : "ffmpeg:  " + local.section('-', 0, 0) + "  →  " + latest);
            m_applyQueue << "ffmpeg";
        }
    }

    if (pending.isEmpty()) emit updateStatus("All tools up to date.");
    emit checkFinished(pending);
}

// ---------------------------------------------------------------------------
// Phase 2 — apply. Async downloads, driven one tool at a time off m_applyQueue.
// ---------------------------------------------------------------------------

void SgUpdater::applyUpdates() {
    if (m_applyQueue.isEmpty()) {
        emit applyFinished(true);
        return;
    }
    m_applyOk = true;
    applyNext();
}

void SgUpdater::applyNext() {
    if (m_applyQueue.isEmpty()) {
        emit updateStatus(m_applyOk ? "Tool updates finished." : "Tool updates finished with errors.");
        emit applyFinished(m_applyOk);
        return;
    }
    const QString tool = m_applyQueue.takeFirst();
    if (tool == "yt-dlp") {
        emit updateStatus("Downloading yt-dlp...");
        downloadNewExe("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe");
    }
    else if (tool == "deno") {
        emit updateStatus("Downloading Deno...");
        downloadNewDeno("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip");
    }
    else {
        downloadFfmpeg();
    }
}

void SgUpdater::downloadFfmpeg() {
    QString toolsDir = QCoreApplication::applicationDirPath() + "/tools";
    QDir().mkpath(toolsDir);
    QString zipPath = toolsDir + "/ffmpeg_update.zip";

    emit updateStatus("Downloading ffmpeg...");

    // Open the file on disk now and stream bytes into it as they arrive.
    // This avoids buffering a 100MB+ zip entirely in memory.
    QFile* zipFile = new QFile(zipPath, this);
    if (!zipFile->open(QIODevice::WriteOnly)) {
        emit updateStatus("Failed to open ffmpeg zip for writing.");
        delete zipFile;
        m_applyOk = false;
        applyNext();
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
            m_applyOk = false;
            applyNext();
            return;
        }

        // The essentials zip is ~103 MB. Anything under 90 MB means a truncated
        // or redirected-to-HTML download — reject it so we never extract garbage.
        QFileInfo info(zipPath);
        if (info.size() < 90000000) {
            emit updateStatus("ffmpeg zip incomplete (" + QString::number(info.size() / 1024 / 1024) + " MB, expected ~103 MB) — aborting.");
            QFile::remove(zipPath);
            reply->deleteLater();
            m_applyOk = false;
            applyNext();
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
            m_applyOk = false;
            applyNext();
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
                m_applyOk = false;
                applyNext();
                return;
            }

            emit updateStatus("ffmpeg installed ("
                + QString::number(ffmpegSz / 1024 / 1024) + " MB), ffprobe ("
                + QString::number(ffprobeSz / 1024 / 1024) + " MB).");
            applyNext();
            });
        ps->start("powershell", { "-NoProfile", "-NonInteractive", "-Command", psCmd });
        });
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
    if (total <= 0) return;
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    // downloadProgress fires for every network chunk — thousands of times for
    // the ffmpeg zip. Forwarding each one queues two cross-thread signals (and
    // a Queue-console append) on the UI thread, which floods its event queue
    // and freezes the window for the length of the download. Only forward
    // whole-percent changes, and log every 5%.
    int pct = static_cast<int>(received * 100 / total);
    if (pct == reply->property("lastPct").toInt() && reply->property("lastPct").isValid())
        return;
    reply->setProperty("lastPct", pct);

    QString tool = "tool";
    QString type = reply->property("downloadType").toString();
    if (type == "yt-dlp-exe")        tool = "yt-dlp";
    if (type == "deno-zip")          tool = "Deno";
    if (type == "ffmpeg-zip-stream") tool = "ffmpeg";

    emit applyProgress(tool, pct);
    if (pct % 5 == 0)
        emit updateStatus("Downloading " + tool + ": " + QString::number(pct) + "%");
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
        m_applyOk = false;
        applyNext();
        return;
    }

    if (fileData.size() < 5000000) {
        emit updateStatus("Download too small — aborting update.");
        m_applyOk = false;
        applyNext();
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
            m_applyOk = false;
            applyNext();
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
            m_applyOk = false;
            applyNext();
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
            m_applyOk = false;
        }

        applyNext();
    }
    else if (downloadType == "deno-zip") {
        QString zipPath = toolsDir + "/deno_temp.zip";
        QString exePath = toolsDir + "/deno.exe";

        QFile file(zipPath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit updateStatus("Failed to write Deno zip to disk.");
            m_applyOk = false;
            applyNext();
            return;
        }
        file.write(fileData);
        file.close();

        // Verify zip against Deno's published .sha256sum. Deno generates it with
        // PowerShell Get-FileHash, so the body is a Format-List block ("Algorithm :
        // SHA256 / Hash : <HEX> / Path : ..."), NOT the usual "hash  filename" line.
        // Pull the 64-char hex digest out by pattern so either format works.
        emit updateStatus("Verifying Deno hash...");
        QString sumBody = fetchRemoteText("https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum");
        QString expected = QRegularExpression("[0-9a-fA-F]{64}").match(sumBody).captured(0);
        if (!verifyHash(zipPath, expected, "Deno")) {
            QFile::remove(zipPath);
            m_applyOk = false;
            applyNext();
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
            m_applyOk = false;
        }

        applyNext();
    }
}
