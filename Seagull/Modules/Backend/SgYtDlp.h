#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QUrl>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include "SgFormat.h" // StreamOption + format-selection policy

class SgHlsProxy;
class SgMetaCache;

// Thin wrapper around the yt-dlp.exe process. Runs one job at a time (download /
// metadata+streamURL / quality probe / playlist), buffers the output, parses the
// `-J` JSON, and emits the results. The orchestrator runs several instances so
// their long-running processes never step on each other. Format selection lives
// in SgFormat; download-arg policy in SgOptions; tool updates in SgUpdater.
class SgYtDlp : public QObject {
    Q_OBJECT
public:
    explicit SgYtDlp(QObject* parent = nullptr);
    ~SgYtDlp();

    void download(const QString& url);
    // freshResolve bypasses the metadata cache (used when a cached stream URL went
    // stale and VLC failed to open it).
    void fetchMetadataAndStreamUrl(const QString& url, const QString& formatId = QString(), bool freshResolve = false);
    void probeAvailableQualities(const QString& url);
    void fetchPlaylistEntries(const QString& playlistUrl);
    // Fetch up to maxComments (top) comments for a video page as a separate, bounded
    // job — comment extraction is slow/paginated, so it never rides along with the
    // play-path probe. Run it on its own worker. yt-dlp has no offset, so "load more"
    // re-requests a larger window; the caller de-dupes by comment id. Emits
    // commentsReady; empty array = none / unsupported.
    void fetchComments(const QString& url, int maxComments);
    void cancel();

    // Optional shared ad-stripping HLS proxy. When set, a resolved Twitch *live*
    // stream URL is routed through it so VLC never sees the stitched ad segments.
    void setHlsProxy(SgHlsProxy* proxy) { m_hlsProxy = proxy; }

    // Optional shared -J cache. When set, every worker reads/writes the same cache
    // so the same video isn't re-extracted by the resolver, prefetcher, and player
    // independently (a request burst YouTube flags as bot traffic). Unset = the
    // local per-instance cache below is used.
    void setMetaCache(SgMetaCache* cache) { m_metaCacheShared = cache; }

signals:
    void logMessage(const QString& message);
    void progressUpdated(double percentage);
    // Richer download progress for the Download Manager: percent plus yt-dlp's own speed
    // ("1.50MiB/s") and ETA ("00:03") strings (either may be empty on a given line).
    void downloadProgress(double percentage, const QString& speed, const QString& eta);
    // The output file yt-dlp is writing to, parsed from its Destination/Merger lines, so a
    // finished download can offer "Open folder". Emitted during a Downloading job.
    void downloadDestination(const QString& path);
    void finished(bool success);
    void metadataReady(const QString& title, const QString& uploader, const QString& duration,
        const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);
    void availableQualitiesFound(const QList<StreamOption>& options);
    void thumbnailResolved(const QString& thumbUrl);
    // True when yt-dlp reports the source is a live stream. Emitted from the probe
    // (which runs on every play) so the player can render a LIVE seeker/timestamp.
    void liveStatusKnown(bool isLive);
    // Full metadata for the player's Info panel (includes the description).
    void videoInfoReady(const QString& title, const QString& uploader,
        const QString& views, const QString& date, const QString& description);
    // The video's comment_count straight from the probe (free — no comment extraction).
    // Lets the shell decide whether to offer a Comments tab before fetching anything.
    void commentCountKnown(int count);
    void streamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl = QUrl());
    void playlistEntriesReady(const QList<QString>& urls);
    // The video's comments (yt-dlp `comments` array: author/text/like_count/timestamp/
    // parent/author_is_uploader per entry) and its total comment_count. Empty array
    // when the source has none or doesn't support comment extraction.
    void commentsReady(const QJsonArray& comments, int totalCount);
    // A job failed in a way that looks like the source (usually YouTube) is blocking
    // us: bot-check ("Sign in to confirm you're not a bot") or rate-limiting / throttling
    // (HTTP 429). kind is "bot" or "throttle"; detail is the trimmed yt-dlp error line.
    // The orchestrator turns this into a single debounced warning modal.
    void extractionBlocked(const QString& kind, const QString& detail);

private slots:
    void handleReadyRead();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void emitProbeResults(const QJsonObject& root); // thumbnail / qualities / live / info
    void processMetadata(const QJsonObject& obj);   // the above + metadataReady + stream resolve

    // Emits logMessage AND mirrors the line into the verbose log (SgLog). Every
    // internal log point routes through here, so all yt-dlp activity is captured
    // when verbose logging is on, regardless of which worker produced it.
    void logLine(const QString& message);

    // -J results are cached per URL so a quality switch / replay / re-probe within
    // the TTL is answered locally (no yt-dlp relaunch — the slow part of starting a
    // VOD). Live streams are never cached (their URLs rotate).
    QJsonObject cachedMetadata(const QString& url);              // fresh entry or empty
    void storeMetadata(const QString& url, const QJsonObject& obj);

    QProcess* m_process;

    enum class JobMode { Idle, Downloading, FetchingMetadata, Probing, FetchingPlaylist, FetchingComments };
    JobMode currentMode = JobMode::Idle;

    QByteArray processBuffer;

    // The chosen video format id (empty = honor the default Stream Quality
    // setting) is stashed here while the -J resolve runs.
    QString m_pendingFormatId;

    // URL of the in-flight -J job, so the result can be cached when it lands.
    QString m_pendingMetaUrl;

    struct MetaCacheEntry { QJsonObject obj; qint64 atMs; };
    QHash<QString, MetaCacheEntry> m_metaCache; // per-instance fallback when no shared cache is set

    SgHlsProxy* m_hlsProxy = nullptr;       // shared, owned by the orchestrator (may be null)
    SgMetaCache* m_metaCacheShared = nullptr; // shared -J cache (may be null -> use m_metaCache)
};
