#include "SgAppUpdate.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>

// Stamped in by the build (see CMakeLists). Fallback keeps a stray build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

namespace {
// /releases (not /releases/latest): the latter skips pre-releases, and right now
// every Seagull release is a beta. Newest is first in the returned list.
constexpr const char* kReleasesApi =
    "https://api.github.com/repos/BrostrodamusTheWeez0rd/Seagull/releases?per_page=10";
}

SgAppUpdate::SgAppUpdate(QObject* parent) : QObject(parent) {
    m_nam = new QNetworkAccessManager(this);
}

void SgAppUpdate::checkForUpdate() {
    QNetworkRequest req{ QUrl(QString::fromLatin1(kReleasesApi)) };
    req.setRawHeader("User-Agent", "Seagull-Player");            // GitHub rejects UA-less requests
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonArray releases = QJsonDocument::fromJson(reply->readAll()).array();
        if (releases.isEmpty()) { emit upToDate(); return; } // no releases published

        // Newest published release (drafts aren't visible unauthenticated, and the
        // list is newest-first), pre-releases included for the beta channel.
        const QJsonObject rel = releases.first().toObject();
        const QString notes = rel.value("body").toString();
        const QString url   = rel.value("html_url").toString();
        QString remote      = rel.value("tag_name").toString();
        if (remote.startsWith('v') || remote.startsWith('V')) remote = remote.mid(1);

        // Remember the downloadable build (first .zip asset) for downloadAndApply().
        m_assetUrl.clear();
        const QJsonArray assets = rel.value("assets").toArray();
        for (const QJsonValue& a : assets) {
            const QJsonObject ao = a.toObject();
            if (ao.value("name").toString().endsWith(".zip", Qt::CaseInsensitive)) {
                m_assetUrl = ao.value("browser_download_url").toString();
                break;
            }
        }

        if (isNewer(remote, QString::fromLatin1(SEAGULL_VERSION)))
            emit updateAvailable(remote, notes, url);
        else
            emit upToDate();
    });
}

void SgAppUpdate::downloadAndApply() {
    if (m_assetUrl.isEmpty()) {
        emit downloadFailed("This release has no downloadable build attached.");
        return;
    }

    // Fresh staging area under temp; never touch the install until verified.
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("seagull_update"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);
    const QString zipPath = base + "/update.zip";

    auto* file = new QFile(zipPath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        file->deleteLater();
        emit downloadFailed("Could not write to the temp folder.");
        return;
    }

    QNetworkRequest req{ QUrl(m_assetUrl) };
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->get(req);

    connect(reply, &QNetworkReply::downloadProgress, this, &SgAppUpdate::downloadProgress);
    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, base, zipPath]() {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        file->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit downloadFailed(reply->errorString());
            return;
        }

        // Extract with tar (ships with Windows 10 1803+; creates/reads real zips).
        const QString staged = base + "/staged";
        QDir().mkpath(staged);
        QProcess tar;
        tar.start("tar", { "-xf", QDir::toNativeSeparators(zipPath),
                           "-C",  QDir::toNativeSeparators(staged) });
        if (!tar.waitForStarted(5000) || !tar.waitForFinished(120000) || tar.exitCode() != 0) {
            emit downloadFailed("Could not extract the downloaded update.");
            return;
        }

        // The release zip carries a top-level Seagull\ folder; tolerate a flat zip too.
        QString appRoot = staged + "/Seagull";
        if (!QFile::exists(appRoot + "/Seagull.exe")) {
            if (QFile::exists(staged + "/Seagull.exe")) appRoot = staged;
            else { emit downloadFailed("The downloaded update looks incomplete."); return; }
        }
        emit readyToApply(appRoot);
    });
}

bool SgAppUpdate::isNewer(const QString& remote, const QString& local) {
    // Split "X.Y.Z-pre" into the three numbers + the pre-release tail.
    auto parse = [](QString v, int nums[3], QString& pre) {
        v = v.trimmed();
        if (v.startsWith('v') || v.startsWith('V')) v = v.mid(1);
        const int dash = v.indexOf('-');
        const QString core = (dash >= 0) ? v.left(dash) : v;
        pre = (dash >= 0) ? v.mid(dash + 1) : QString();
        const QStringList parts = core.split('.');
        for (int i = 0; i < 3; ++i) nums[i] = (i < parts.size()) ? parts.at(i).toInt() : 0;
    };

    int r[3], l[3];
    QString rp, lp;
    parse(remote, r, rp);
    parse(local, l, lp);

    for (int i = 0; i < 3; ++i)
        if (r[i] != l[i]) return r[i] > l[i];

    // Same X.Y.Z: a final release outranks a pre-release of the same core.
    if (rp.isEmpty() && lp.isEmpty()) return false; // identical
    if (rp.isEmpty()) return true;                  // remote final  > local pre
    if (lp.isEmpty()) return false;                 // remote pre     < local final
    return rp.compare(lp) > 0;                       // both pre: lexical (beta < beta.2 < rc)
}
