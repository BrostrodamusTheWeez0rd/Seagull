#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QUrl>
#include <QHash>
#include <QJsonObject>
#include "SgFormat.h" // StreamOption + format-selection policy

class SgHlsProxy;

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
    void cancel();

    // Optional shared ad-stripping HLS proxy. When set, a resolved Twitch *live*
    // stream URL is routed through it so VLC never sees the stitched ad segments.
    void setHlsProxy(SgHlsProxy* proxy) { m_hlsProxy = proxy; }

signals:
    void logMessage(const QString& message);
    void progressUpdated(double percentage);
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
    void streamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl = QUrl());
    void playlistEntriesReady(const QList<QString>& urls);
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

    // -J results are cached per URL so a quality switch / replay / re-probe within
    // the TTL is answered locally (no yt-dlp relaunch — the slow part of starting a
    // VOD). Live streams are never cached (their URLs rotate).
    QJsonObject cachedMetadata(const QString& url);              // fresh entry or empty
    void storeMetadata(const QString& url, const QJsonObject& obj);

    QProcess* m_process;

    enum class JobMode { Idle, Downloading, FetchingMetadata, Probing, FetchingPlaylist };
    JobMode currentMode = JobMode::Idle;

    QByteArray processBuffer;

    // The chosen video format id (empty = honor the default Stream Quality
    // setting) is stashed here while the -J resolve runs.
    QString m_pendingFormatId;

    // URL of the in-flight -J job, so the result can be cached when it lands.
    QString m_pendingMetaUrl;

    struct MetaCacheEntry { QJsonObject obj; qint64 atMs; };
    QHash<QString, MetaCacheEntry> m_metaCache;

    SgHlsProxy* m_hlsProxy = nullptr; // shared, owned by the orchestrator (may be null)
};
