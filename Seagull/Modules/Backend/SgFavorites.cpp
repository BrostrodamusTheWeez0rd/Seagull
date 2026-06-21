#include "SgFavorites.h"
#include "SgPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>

SgFavorites* SgFavorites::instance() {
    static SgFavorites* s_instance = new SgFavorites(nullptr);
    return s_instance;
}

SgFavorites::SgFavorites(QObject* parent) : QObject(parent) {
    load();
    m_nam = new QNetworkAccessManager(this);
}

bool SgFavorites::isFavorited(const QString& channelUrl) const {
    return m_favorites.contains(channelUrl);
}

void SgFavorites::setFavorited(const QString& channelUrl, const QString& channelName,
                                const QString& thumbnailUrl, bool favorited) {
    if (favorited) {
        const bool alreadyPresent = m_favorites.contains(channelUrl);
        if (alreadyPresent
            && m_favorites.value(channelUrl) == channelName
            && m_thumbnailUrls.value(channelUrl) == thumbnailUrl)
            return; // already stored with the same data — no-op
        if (!alreadyPresent)
            m_order.append(channelUrl); // new entry: add to the ordered list
        m_favorites.insert(channelUrl, channelName);
        m_thumbnailUrls.insert(channelUrl, thumbnailUrl);
        save();
        if (!thumbnailUrl.isEmpty() && !QFile::exists(thumbCachePath(channelUrl)))
            downloadThumbnail(channelUrl, thumbnailUrl);
        else if (thumbnailUrl.isEmpty() && !alreadyPresent)
            onAvatarNeeded(channelUrl); // video-card star: fetch avatar via yt-dlp
    } else {
        if (!m_favorites.contains(channelUrl))
            return; // not present — no-op
        m_favorites.remove(channelUrl);
        m_thumbnailUrls.remove(channelUrl);
        m_order.removeOne(channelUrl);
        const QString cachePath = thumbCachePath(channelUrl);
        if (QFile::exists(cachePath))
            QFile::remove(cachePath);
        save();
    }
    emit changed(channelUrl, favorited);
}

QList<SgFavorites::FavoriteChannel> SgFavorites::favorites() const {
    QList<FavoriteChannel> result;
    result.reserve(m_order.size());
    for (const QString& url : m_order) {
        FavoriteChannel fc;
        fc.url          = url;
        fc.name         = m_favorites.value(url);
        fc.thumbnailUrl = m_thumbnailUrls.value(url);
        const QString cached = thumbCachePath(url);
        fc.cachedThumbPath = QFile::exists(cached) ? cached : QString();
        result.append(fc);
    }
    return result;
}

void SgFavorites::toggle(const QString& channelUrl, const QString& channelName,
                          const QString& thumbnailUrl) {
    setFavorited(channelUrl, channelName, thumbnailUrl, !isFavorited(channelUrl));
}

void SgFavorites::load() {
    const QString path = SgPaths::configDir() + "/favorites.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return; // no file yet — start empty

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue& v : doc.array()) {
        const QJsonObject obj  = v.toObject();
        const QString url      = obj.value("url").toString();
        const QString name     = obj.value("name").toString();
        const QString thumbUrl = obj.value("thumbnailUrl").toString();
        if (!url.isEmpty() && !m_favorites.contains(url)) {
            m_favorites.insert(url, name);
            m_thumbnailUrls.insert(url, thumbUrl);
            m_order.append(url);
        }
    }
}

void SgFavorites::save() const {
    const QString dir  = SgPaths::configDir();
    const QString path = dir + "/favorites.json";

    QDir().mkpath(dir); // ensure Config/ exists (matches SgPaths::configFile() callers)

    QJsonArray arr;
    for (const QString& url : m_order) {
        QJsonObject obj;
        obj["url"]          = url;
        obj["name"]         = m_favorites.value(url);
        obj["thumbnailUrl"] = m_thumbnailUrls.value(url);
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// Thumbnail cache
// ---------------------------------------------------------------------------

QString SgFavorites::thumbCacheDir() {
    return SgPaths::configDir() + "/channel_thumbnails";
}

QString SgFavorites::thumbCachePath(const QString& channelUrl) {
    const QByteArray hash = QCryptographicHash::hash(channelUrl.toUtf8(), QCryptographicHash::Md5).toHex();
    return thumbCacheDir() + "/" + QString::fromLatin1(hash);
}

void SgFavorites::downloadThumbnail(const QString& channelUrl, const QString& thumbnailUrl) {
    QDir().mkpath(thumbCacheDir());

    QNetworkRequest req((QUrl(thumbnailUrl)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_nam->get(req);
    const QString destPath = thumbCachePath(channelUrl);
    connect(reply, &QNetworkReply::finished, this, [reply, destPath]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) return;
        QFile f(destPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(data);
    });
}

// ---------------------------------------------------------------------------
// Async avatar fetch via yt-dlp (called when starred from a video card)
// ---------------------------------------------------------------------------

void SgFavorites::onAvatarNeeded(const QString& channelUrl) {
    // Don't start a second fetch for the same channel if one is already running.
    if (m_avatarProcesses.values().contains(channelUrl)) return;
    // Also skip if the cache already exists (a previous session may have fetched it).
    if (QFile::exists(thumbCachePath(channelUrl))) return;

    const QString ytDlp = QCoreApplication::applicationDirPath() + "/Tools/yt-dlp.exe";
    auto* proc = new QProcess(this);
    m_avatarProcesses.insert(proc, channelUrl);

    const QStringList args = {
        channelUrl,
        "--flat-playlist",
        "-J",
        "--playlist-end", "1",
        "--no-warnings",
    };

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc]() {
        const QString url = m_avatarProcesses.value(proc);
        m_avatarProcesses.remove(proc);
        // Read stdout before scheduling deletion.
        const QByteArray out = proc->readAllStandardOutput();
        proc->deleteLater();

        if (url.isEmpty()) return;
        // Still favorited? (user might have un-starred during the fetch)
        if (!m_favorites.contains(url)) return;
        const QJsonObject root = QJsonDocument::fromJson(out).object();

        // The root object's "thumbnails" array contains the channel's avatar images.
        // Prefer a square image (width == height = profile picture); fall back to
        // the entry with the smallest width.
        const QJsonArray thumbs = root.value("thumbnails").toArray();
        QString avatarUrl;
        int smallestW = -1; // -1 = no non-square candidate found yet
        for (const QJsonValue& v : thumbs) {
            const QJsonObject t = v.toObject();
            const int w = t.value("width").toInt(-1);
            const int h = t.value("height").toInt(-1);
            const QString u = t.value("url").toString();
            if (u.isEmpty()) continue;
            if (w >= 0 && h >= 0 && w == h) {
                // Square thumbnail — ideal channel avatar; take the first one found.
                avatarUrl = u;
                break;
            }
            // Non-square fallback: track the entry with the smallest known width.
            if (w >= 0 && (smallestW < 0 || w < smallestW)) {
                smallestW = w;
                avatarUrl = u;
            } else if (w < 0 && avatarUrl.isEmpty()) {
                avatarUrl = u; // no dimension info — last resort
            }
        }

        if (avatarUrl.isEmpty()) return;

        // Store the avatar URL in the persistent map and kick off the download.
        m_thumbnailUrls.insert(url, avatarUrl);
        save();
        downloadThumbnail(url, avatarUrl);
    });

    proc->start(ytDlp, args);
}
