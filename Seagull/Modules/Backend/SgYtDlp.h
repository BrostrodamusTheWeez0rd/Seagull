#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QUrl>
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
    void fetchMetadataAndStreamUrl(const QString& url, const QString& formatId = QString());
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

private slots:
    void handleReadyRead();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QProcess* m_process;

    enum class JobMode { Idle, Downloading, FetchingMetadata, Probing, FetchingPlaylist };
    JobMode currentMode = JobMode::Idle;

    QByteArray processBuffer;

    // The chosen video format id (empty = honor the default Stream Quality
    // setting) is stashed here while the -J resolve runs.
    QString m_pendingFormatId;

    SgHlsProxy* m_hlsProxy = nullptr; // shared, owned by the orchestrator (may be null)
};
