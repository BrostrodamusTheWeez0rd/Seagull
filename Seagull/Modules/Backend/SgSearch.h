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
    bool    isLive    = false; // currently-live channel (Twitch) — the card gets a LIVE badge
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
    enum class Site { YouTube, PornHub, Chaturbate, SoundCloud, Twitch };

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
    // shorts=true targets the channel's /shorts tab instead of /videos. PornHub channels
    // ignore it (no shorts). The home feed's Shorts mode uses fetchChannelShorts (below)
    // instead, since yt-dlp can't list a channel's /shorts tab reliably.
    // SoundCloud artist URLs route here too: the same yt-dlp flat listing, targeting
    // the artist's /tracks tab. Live-only sites (Twitch, Chaturbate) have no video
    // listing and answer with a clean `failed` instead of a doomed yt-dlp run.
    void fetchChannelVideos(const QString& channelUrl, int limit = 30, bool shorts = false);

    // Twitch home feed: ONE batched live-status lookup for the favourited channels.
    // `channels` are the favourites' base cards (url/name/thumbnail), built by the
    // caller; the reply enriches them with live status/title/viewer count, orders
    // them live-first (favourites order within each group), and answers on
    // channelVideosReady so the home build consumes it like a single channel batch.
    // On any failure the seeds are emitted back unchanged, so the feed still shows.
    void fetchTwitchChannels(const QList<SearchResult>& channels);

    // Home-feed Shorts: pull up to `limit` Shorts for one favourite channel by
    // searching its name (yt-dlp can't list a channel's /shorts tab reliably), tagging
    // each result with the channel's name/url. Reuses the Shorts search API path but
    // answers on channelVideosReady, so the home build consumes it exactly like /videos.
    void fetchChannelShorts(const QString& channelUrl, const QString& channelName, int limit);

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
    // Finish a Shorts pull: emit resultsReady (live search) or, in home mode, tag the
    // results with the favourite's name/url and emit channelVideosReady instead.
    void emitShortsDone();
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

    // PornHub model/channel listing: the uploader's /videos tab, scraped the same way
    // as search (its tiles use the same markup, so parsePornHubHtml is reused).
    // Answers on channelVideosReady, so the home feed + channel view consume it like
    // the YouTube path. Reached from fetchChannelVideos() when the URL is a PornHub one.
    void fetchPornHubModel(const QString& modelUrl, int limit);
    void handlePornHubModelReply();

    // Chaturbate search (live cam rooms via their JSON room-list API; query = tag).
    void startChaturbateSearch(const QString& query, int limit);
    void fetchChaturbatePage();
    void handleChaturbateReply();
    QList<SearchResult> parseChaturbateJson(const QByteArray& bytes) const;

    // Twitch channel search via the public GQL endpoint (gql.twitch.tv with the
    // anonymous web Client-Id — the same approach yt-dlp and streamlink use; no
    // account or OAuth involved). Single page, cached per query like channel search.
    void startTwitchSearch(const QString& query, int limit);
    void handleTwitchSearchReply();
    void handleTwitchUsersReply();
    QNetworkReply* postTwitchGql(const QJsonObject& body);
    // One GQL User object -> a live-room SearchResult; false if unusable.
    bool parseTwitchUser(const QJsonObject& user, SearchResult& out) const;
    // ".../twitch.tv/<login>/..." -> "login" (lowercased), empty if not a channel URL.
    static QString twitchLoginFromUrl(const QString& url);

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
    // Home-feed mode for the Shorts path: when set, the pull is one favourite channel's
    // Shorts for the landing feed, so it answers on channelVideosReady (tagged with the
    // channel name/url below) rather than the live-view resultsReady.
    bool                   m_shortsHome = false;
    QString                m_shortsHomeUrl;
    QString                m_shortsHomeName;

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

    // PornHub model/channel listing state (single page, scraped on demand).
    QNetworkReply*         m_phModelReply = nullptr;
    QString                m_phModelUrl;            // the model/channel page being listed
    QString                m_phModelName;           // slug-derived fallback display name

    // Chaturbate search state (JSON room-list API; offset paging, per query).
    QNetworkReply*         m_cbReply = nullptr;
    QString                m_cbQuery;
    QList<SearchResult>    m_cbResults;
    QSet<QString>          m_cbSeen;
    int                    m_cbLimit = 20;
    bool                   m_cbExhausted = false;

    // Twitch search state (GQL; single page, cached per query).
    QNetworkReply*         m_twReply = nullptr;
    QString                m_twQuery;
    QList<SearchResult>    m_twResults;
    QSet<QString>          m_twSeen;
    int                    m_twLimit = 20;

    // Twitch home-feed live-status lookup (one batched request per feed build).
    QNetworkReply*         m_twUsersReply = nullptr;
    QList<SearchResult>    m_twHomeSeeds;  // the favourites' base cards, awaiting enrichment
};
