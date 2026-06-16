#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QSet>

class QJsonObject;
class QJsonValue;
class QNetworkAccessManager;
class QNetworkReply;

// One search hit, site-agnostic. Durations are in seconds (-1 unknown), view
// counts -1 when unknown. url is the page URL the player/queue can consume.
struct SearchResult {
    QString title;
    QString url;
    QString channel;
    QString thumbnail;
    QString thumbnailReferer; // Referer header to fetch the thumbnail (hotlink-protected CDNs); empty = none
    qint64  duration  = -1;
    qint64  viewCount = -1;
    qint64  timestamp = -1;   // approximate upload time (Unix seconds), -1 unknown
    int     seq       = 0;    // arrival order, set by the Search UI; "Relevance" sort key
    bool    isShort   = false;
    // Channel support. isChannel marks a channel result (the tile becomes a
    // channel card). channelUrl is the channel/uploader page: on a video result it
    // makes the uploader name clickable; on a channel result it's the page to open.
    // subscriberCount is the channel's follower count (-1 unknown).
    bool    isChannel = false;
    QString channelUrl;
    qint64  subscriberCount = -1;
    // Ready-made subscriber text ("15.5M subscribers") from the channel-search API,
    // which gives it pre-formatted; preferred over subscriberCount when set.
    QString subscriberText;
};

// Backend search worker — a peer to SgYtDlp, dedicated to discovery rather than
// download/stream resolution. Wraps the yt-dlp process for now; the SearchSite
// enum is the seam for adding more sites later (each gets its own arg-building
// and result-parsing branch). Runs one query at a time, async via QProcess.
//
// Shorts are the exception: yt-dlp's search extractor can't parse YouTube's
// shorts result renderers (shortsLockupViewModel) at all, so shortsOnly
// searches go straight to YouTube's internal search API over QNetworkAccess-
// Manager instead, paging via continuation tokens. Pages accumulate per query
// so the UI's grow-the-limit "load more" calls only fetch what's missing.
class SgSearch : public QObject {
    Q_OBJECT
public:
    enum class Site { YouTube, PornHub };

    explicit SgSearch(QObject* parent = nullptr);
    ~SgSearch();

    void search(Site site, const QString& query, int limit = 20, bool shortsOnly = false);

    // Search for channels by name (the "channel:" prefix). yt-dlp can't reliably
    // return channel results, so this goes through YouTube's internal search API
    // (like the shorts path). Answers on the normal resultsReady with channel
    // SearchResults (isChannel=true). Single page — channel searches don't paginate.
    void searchChannels(const QString& query, int limit = 20);

    // List a channel's uploads (its /videos tab) via yt-dlp, up to `limit`. The
    // root object yields the channel header (name, avatar, subscribers); the
    // entries are normal video results. Answers on channelVideosReady. Paging
    // works like search: call again with a larger limit.
    void fetchChannelVideos(const QString& channelUrl, int limit = 30);

    void cancel();

signals:
    void resultsReady(const QList<SearchResult>& results);
    void channelVideosReady(const SearchResult& channelInfo, const QList<SearchResult>& videos);
    void failed(const QString& message);
    void logMessage(const QString& message);

private slots:
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    // What the shared QProcess is currently doing, so handleFinished parses + emits
    // the right thing.
    enum class Mode { VideoSearch, ChannelList };

    QList<SearchResult> parseYoutube(const QJsonObject& root) const;
    // Parse one flat video entry into `out`; false if it isn't a usable video.
    bool parseVideoEntry(const QJsonObject& e, SearchResult& out) const;
    // Channel listing root -> header info + video results.
    void parseChannelList(const QJsonObject& root, SearchResult& info,
                          QList<SearchResult>& videos) const;

    void startShortsSearch(const QString& query, int limit);
    void fetchShortsPage();
    void handleShortsReply();
    void fetchChannelSearchPage();
    void handleChannelSearchReply();
    static void collectObjects(const QJsonValue& v, const QString& key,
                               QList<QJsonObject>& out);

    // PornHub search (HTML scrape of the results page; one request per page). yt-dlp's
    // flat search returns only title+url for PornHub, so we fetch the results page
    // ourselves and parse the tiles for thumbnails/duration/views. A static
    // age-gate cookie (a constant flag, never the user's browser cookies) avoids the
    // 18+ interstitial.
    void startPornHubSearch(const QString& query, int limit);
    void fetchPornHubPage();
    void handlePornHubReply();
    QList<SearchResult> parsePornHubHtml(const QString& html) const;

    QProcess*  m_process;
    QByteArray m_buffer;
    Site       m_site = Site::YouTube;
    Mode       m_mode = Mode::VideoSearch; // what the current m_process run is doing
    bool       m_cancelled = false; // suppresses the finished handler for a killed query

    // Shorts search state (YouTube internal API path). Results accumulate per
    // query across continuation pages; a new query resets the lot.
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply*         m_shortsReply = nullptr;
    QString                m_shortsQuery;
    QList<SearchResult>    m_shortsResults;
    QSet<QString>          m_shortsSeenIds;       // de-dupe across pages
    QString                m_shortsContinuation;  // next-page token, empty = none yet/exhausted
    bool                   m_shortsExhausted = false;
    int                    m_shortsLimit = 20;

    // Channel search state (YouTube internal API path, single page, cached per query).
    QNetworkReply*         m_channelReply = nullptr;
    QString                m_channelQuery;
    QList<SearchResult>    m_channelResults;
    QSet<QString>          m_channelSeenIds;
    int                    m_channelLimit = 20;

    // PornHub search state (HTML scrape; results accumulate per query across pages).
    QNetworkReply*         m_phReply = nullptr;
    QString                m_phQuery;
    QList<SearchResult>    m_phResults;
    QSet<QString>          m_phSeen;
    int                    m_phPage = 0;            // last results page fetched (1-based)
    int                    m_phLimit = 20;
    bool                   m_phExhausted = false;
};
