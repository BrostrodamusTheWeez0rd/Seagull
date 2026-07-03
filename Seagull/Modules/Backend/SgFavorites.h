#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

class QNetworkAccessManager;
class QProcess;

// Singleton that persists a set of favorited channels to a JSON file under
// Config/. Keyed by channelUrl; stores the display name and thumbnail URL too.
//
// There are five independent, contained stores backed by the SAME class:
//   - instance()   -> YouTube favourites    (Config/favorites.json, avatars via yt-dlp)
//   - phInstance() -> PornHub favourites    (Config/ph_favorites.json, no yt-dlp)
//   - cbInstance() -> Chaturbate favourites (Config/cb_favorites.json, no yt-dlp)
//   - scInstance() -> SoundCloud favourites (Config/sc_favorites.json, avatars via yt-dlp)
//   - twInstance() -> Twitch favourites     (Config/tw_favorites.json, no yt-dlp)
// The first call to each creates that singleton and loads its JSON; every toggle
// writes back immediately. Use forUrl() to route a card's channel URL to its store.
class SgFavorites : public QObject {
    Q_OBJECT
public:
    struct FavoriteChannel {
        QString url;
        QString name;
        QString thumbnailUrl;    // original network URL (empty if unknown)
        QString cachedThumbPath; // local file path if downloaded, else empty
    };

    static SgFavorites* instance();   // YouTube store
    static SgFavorites* phInstance(); // PornHub store
    static SgFavorites* cbInstance(); // Chaturbate store
    static SgFavorites* scInstance(); // SoundCloud store
    static SgFavorites* twInstance(); // Twitch store

    // Routes a channel/model/artist URL to the store that owns it, or nullptr if
    // the URL belongs to no favouritable site.
    static SgFavorites* forUrl(const QString& url);

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
    // storeFile: filename under Config/ (e.g. "favorites.json").
    // fetchAvatars: if true, a video-card star with no avatar URL triggers a
    // yt-dlp avatar fetch (YouTube + SoundCloud); the live/scraped sites leave
    // it off (their cards already carry the avatar/room image).
    explicit SgFavorites(const QString& storeFile, bool fetchAvatars, QObject* parent = nullptr);

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

    QString m_storeFile;          // JSON filename under Config/
    bool    m_fetchAvatars = true;// gate the yt-dlp avatar fetch (YouTube only)
};
