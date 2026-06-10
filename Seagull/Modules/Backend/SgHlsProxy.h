#ifndef SGHLSPROXY_H
#define SGHLSPROXY_H

#include <QObject>
#include <QUrl>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QHash>
#include <QList>

class QTcpServer;
class QTcpSocket;
class QNetworkAccessManager;

// A localhost HTTP server that gives VLC an ad-free Twitch live stream, browser- and
// cookie-free. The primary method mirrors what the open-source ad-blockers (vaft /
// TwitchAdSolutions) do: instead of scrubbing the ad-stitched playlist yt-dlp resolves
// (which uses playerType="site"), we request our OWN PlaybackAccessToken with
// playerType="embed" (falling back to "popout"/"autoplay"), which Twitch serves a
// clean, un-stitched stream, then resolve usher ourselves and hand VLC that variant.
// As a safety net, the served media playlist still runs through the stitched-ad
// stripper, so if a clean token can't be had we degrade to filtering.
//
// What VLC polls is a re-pollable variant MEDIA playlist (every ~2s); THE invariant is
// every poll returns a valid, non-empty playlist of real, decodable segments
// immediately — never stall, never empty, never synthetic.
//
// Stateful per stream (keyed by the served variant URL), renumbering real segments with
// a monotonic media-sequence so any residual ad-strip never makes VLC replay/snap.
// Bound to 127.0.0.1 with a per-run secret token so nothing else can use it.
class SgHlsProxy : public QObject {
    Q_OBJECT
public:
    explicit SgHlsProxy(QObject* parent = nullptr);

    bool isListening() const;

    // Twitch live: returns a local URL that resolves+serves an ad-free stream for
    // `login` at (or below) `height` via the embed-token method, falling back to
    // filtering `fallbackVariant` (yt-dlp's resolved URL) if that fails. height<=0 = best.
    QUrl proxifyTwitch(const QString& login, int height, const QUrl& fallbackVariant, const QString& referer);

    // Generic: returns a local URL that serves an ad-filtered version of `upstream`.
    QUrl proxify(const QUrl& upstream, const QString& referer);

private:
    struct Seg {
        quint64 seq;       // media-sequence number we assigned this segment
        QString extinf;    // the segment's #EXTINF line
        QString url;       // absolute CDN URL
    };
    struct StreamState {
        quint64 nextSeq = 0;                 // next output media-sequence to hand out
        QHash<QString, quint64> seqOf;       // segment URL -> assigned seq (dedup across refreshes)
        QStringList order;                   // URLs in assignment order, for bounded trimming
        QList<Seg> lastWindow;               // the last real window we advertised (held during a mid-roll)
        bool servingPreroll = false;         // we are serving the join preroll's ad segments
        quint64 discontinuitySeq = ~0ull;    // emit #EXT-X-DISCONTINUITY before this seq (preroll-ad -> live seam)
        int version = 3;
        int targetDuration = 2;
    };

    void onConnection();
    void handleRequest(QTcpSocket* sock, const QString& target);

    // --- Twitch embed-token resolution (primary, ad-free) ---
    void serveTwitch(QTcpSocket* sock, const QString& login, int height,
                     const QString& referer, const QUrl& fallback);
    // Tries playerTypes[idx], idx+1, ... ; on total failure serves `fallback` (filtered).
    void resolveTwitch(QTcpSocket* sock, const QString& login, int height,
                       const QString& referer, const QUrl& fallback, int idx);
    QString pickVariant(const QString& masterText, int height) const;

    // --- serve + ad-strip a media playlist (shared) ---
    void serveProxied(QTcpSocket* sock, const QUrl& upstream, const QString& referer);
    bool isMasterPlaylist(const QString& text) const;
    QByteArray filterMaster(const QString& text, const QUrl& base, const QString& referer) const;
    QByteArray filterMedia(const QString& text, const QUrl& base, StreamState& st);
    QByteArray buildMediaPlaylist(const StreamState& st, const QList<Seg>& window) const;

    QString proxyUrlFor(const QString& absoluteUrl, const QString& referer) const;
    QString localBase() const; // http://127.0.0.1:<port>
    static void writeResponse(QTcpSocket* sock, int code, const QByteArray& contentType, const QByteArray& body);

    QTcpServer* m_server;
    QNetworkAccessManager* m_nam;
    QString m_token;     // random secret guarding the endpoint
    QString m_deviceId;  // random per-run X-Device-Id for the GQL token request
    QHash<QString, StreamState> m_streams;        // served-variant URL -> live filtering state
    QHash<QString, QString> m_twitchVariant;      // "login|height" -> resolved clean variant URL
};

#endif // SGHLSPROXY_H
