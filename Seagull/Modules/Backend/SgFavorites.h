#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

class QNetworkAccessManager;
class QProcess;

// Singleton that persists a set of favorited YouTube channels to
// Config/favorites.json. Keyed by channelUrl; stores the display name and
// thumbnail URL too.
//
// Access via SgFavorites::instance(). The first call creates the singleton
// and loads the JSON from disk. Every toggle writes back immediately.
class SgFavorites : public QObject {
    Q_OBJECT
public:
    struct FavoriteChannel {
        QString url;
        QString name;
        QString thumbnailUrl;    // original network URL (empty if unknown)
        QString cachedThumbPath; // local file path if downloaded, else empty
    };

    static SgFavorites* instance();

    bool isFavorited(const QString& channelUrl) const;
    void setFavorited(const QString& channelUrl, const QString& channelName,
                      const QString& thumbnailUrl, bool favorited);
    void toggle(const QString& channelUrl, const QString& channelName,
                const QString& thumbnailUrl);

    // Returns all favorited channels in insertion order.
    QList<FavoriteChannel> favorites() const;

signals:
    void changed(const QString& channelUrl, bool isFavorited);

private:
    explicit SgFavorites(QObject* parent = nullptr);

    void load();
    void save() const;

    static QString thumbCacheDir();
    static QString thumbCachePath(const QString& channelUrl);
    void downloadThumbnail(const QString& channelUrl, const QString& thumbnailUrl);

    // Async avatar fetch via yt-dlp: invoked when a channel is starred from a
    // video card (no avatar URL is known at that point).
    void onAvatarNeeded(const QString& channelUrl);

    // url -> display name. Presence in the map means favorited.
    QHash<QString, QString>        m_favorites;
    // Tracks insertion order so favorites() returns a stable, predictable list.
    QList<QString>                 m_order;
    // url -> original thumbnail network URL
    QHash<QString, QString>        m_thumbnailUrls;
    QNetworkAccessManager*         m_nam = nullptr;
    // Tracks in-flight avatar-fetch processes so we don't double-fetch.
    // Key: QProcess*, Value: channel URL being fetched.
    QHash<QProcess*, QString>      m_avatarProcesses;
};
