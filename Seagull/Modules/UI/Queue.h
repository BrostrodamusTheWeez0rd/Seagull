#ifndef QUEUE_H
#define QUEUE_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QString>
#include <QPoint>
#include <QPair>
#include <QMap>
#include <QList>
#include <QDateTime>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkReply>

// Forward declaration instead of #include keeps the UI decoupled from the backend implementation
class SgYtDlp;
class QMovie;

class Queue : public QWidget {
    Q_OBJECT
public:
    // The orchestrator passes the shared backend workers in through the constructor.
    explicit Queue(SgYtDlp* downloaderWorker, SgYtDlp* resolverWorker, SgYtDlp* prefetcherWorker, QWidget* parent = nullptr);

    // Sequential playback logic
    void playNextQueuedItem();
    void playPrevQueuedItem();
    void setStreamingQueueMode(bool active);

    // Clears the URL bar once a video starts playing, but leaves the metadata
    // preview up (so it shows the now-playing video until a new link is pasted).
    void clearUrlForPlayback();

    // Entry point for other tabs (e.g. the Search cards) to add a URL + title to
    // the queue table, reusing the queue's existing add flow.
    void addUrlToQueue(const QString& url, const QString& title);

    // Entry point for the Library / File Explorer to queue local files. The queue
    // holds ONE locality at a time (local files or online URLs, never mixed) —
    // adding across that line pops the clear-queue-first modal.
    void addLocalFilesToQueue(const QStringList& paths);

    // Load a .sgpl playlist into the queue (replacing it, confirmed if non-empty);
    // autoPlay starts it immediately (the Library's playlist cards do).
    void loadPlaylistFile(const QString& path, bool autoPlay);

signals:
    // EMITS: Raw URL, CDN Video URL, CDN Audio URL, and Title
    void playMediaRequested(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title);

    // A local queue row wants playing — routed to the player's local-file path.
    void playLocalFileRequested(const QUrl& url);

    // A .sgpl was written to the playlist folder (the shell flashes the Library).
    void playlistSaved(const QString& path);

private slots:
    void onDownloadClicked();
    void onAddToQueueClicked();
    void onProcessQueueClicked();
    void onStreamClicked();
    void onStreamQueueClicked();
    void onCreatePlaylistClicked();
    void onClearQueueClicked();
    void onUrlTextChanged(const QString& text);
    void triggerMetadataFetch();

    // Main downloader callbacks
    void handleMetadataReady(const QString& title, const QString& uploader, const QString& duration,
        const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);
    void handleStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);
    void handleLogMessage(const QString& message);
    void handleProgress(double percentage);
    void handleFinished(bool success);
    void handlePlaylistEntriesReady(const QList<QString>& urls);

    // Background title resolver callbacks
    void handleResolverMetadataReady(const QString& title, const QString& uploader, const QString& duration,
        const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);
    void resolveNextTitle();

    // CDN Pre-fetcher callbacks
    void handlePrefetchedStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);
    void prefetchNextInQueue();

    // Queue management
    void showContextMenu(const QPoint& pos);
    void playSelectedItem();
    void downloadSelectedItems();
    void removeSelectedItems();
    void updateQueueButtonVisibility();

private:
    // The queue's locality. None = empty queue, accepts either kind; once the
    // first entry lands the queue is locked to its kind until cleared.
    enum class QueueKind { None, Local, Online };
    bool ensureQueueKind(QueueKind want); // purity gate; pops the clear-first modal

    bool    isPlaylistUrl(const QString& url) const;
    QString stripToVideoUrl(const QString& url) const;
    void    offerPlaylistQueue(const QString& fullUrl);
    void    playQueueIndex(int index);

    // True if the cached CDN URL is still usable. Checks YouTube's ?expire= token
    // when present; other sites have no checkable token, so any non-empty URL passes.
    bool    isStreamUrlValid(const QUrl& cdnUrl) const;
    void    enqueueTitleResolution(const QList<QString>& urls, int startRow);
    void    resetHeroToBanner();  // restore the big banner, hide the thumbnail hero
    void    showLoading(const QString& text); // show the fetching message + seagull spinner
    void    hideLoading();                     // hide the message and stop the spinner

    // Widgets
    QLabel* banner;
    QLabel* heroThumb;       // big thumbnail shown in the banner's spot once metadata loads
    QLabel* bannerWatermark; // shrunk banner overlaid on the thumbnail's bottom-left
    QLabel* loadingLabel;
    QLabel* m_loadingSpinner;  // shows the animated seagull beside loadingLabel
    QMovie* m_loadingMovie;    // the SeagullAnim.gif driving m_loadingSpinner
    QLineEdit* urlInput;
    QPushButton* downBtn;
    QPushButton* queueBtn;
    QPushButton* streamBtn;
    QPushButton* processQueueBtn;
    QPushButton* streamQueueBtn;
    QPushButton* createPlaylistBtn;
    QPushButton* clearQueueBtn;
    QWidget* metadataContainer;
    QLabel* metaTitle;
    QLabel* metaUploader;
    QLabel* metaStats;
    QTableWidget* queueTable;
    QProgressBar* progressBar;
    QTextEdit* logConsole;

    // Timers
    QTimer* debounceTimer;
    QTimer* resolverTimer;

    // Backends (Now owned by Orchestrator, but referenced here)
    SgYtDlp* downloader;
    SgYtDlp* titleResolver;
    SgYtDlp* cdnPrefetcher;

    // Thumbnail fetching for the metadata preview
    QNetworkAccessManager* m_thumbNam;
    QString m_currentThumbUrl;  // guards against a stale reply painting the wrong thumb

    // State
    bool    isFetchingMetadata = false;
    bool    isProcessingQueue = false;
    bool    isStreamingQueue = false;
    QString cachedTitle;
    QString m_pendingPlaylistUrl;

    // Cache state
    QMap<QString, QPair<QUrl, QUrl>> cdnCache;
    QString m_currentlyPrefetchingUrl;

    // Background title resolution queue
    QList<QPair<int, QString>> m_titleQueue;
    int m_currentResolvingRow = -1;

    // Stream queue playback state
    struct QueueEntry {
        QString rawUrl;        // online: page URL; local: absolute file path
        QUrl    cdnVideoUrl;
        QUrl    cdnAudioUrl;
        QString title;
        bool    local = false; // snapshot of the kind at queue-build time
    };
    QList<QueueEntry> m_streamQueue;
    int m_queuePlayIndex = -1;
    bool m_waitingForCdn = false; // true when playQueueIndex is blocked waiting for a CDN fetch
    QueueKind m_queueKind = QueueKind::None;
};

#endif