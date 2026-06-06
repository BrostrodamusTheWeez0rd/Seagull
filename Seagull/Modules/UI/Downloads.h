#ifndef DOWNLOADS_H
#define DOWNLOADS_H

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
#include <QDateTime>
#include "../Backend/SgYtDlp.h"

class Downloads : public QWidget {
    Q_OBJECT
public:
    explicit Downloads(QWidget* parent = nullptr);

signals:
    // EMITS: Raw URL, CDN Video URL, CDN Audio URL, and Title
    void playMediaRequested(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title);

private slots:
    void onDownloadClicked();
    void onAddToQueueClicked();
    void onProcessQueueClicked();
    void onStreamClicked();
    void onStreamQueueClicked();
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
    bool    isPlaylistUrl(const QString& url) const;
    QString stripToVideoUrl(const QString& url) const;
    void    offerPlaylistQueue(const QString& fullUrl);

    // Validates the Unix timestamp token inside the YouTube CDN URL
    bool    isStreamUrlValid(const QUrl& cdnUrl) const;

    void    enqueueTitleResolution(const QList<QString>& urls, int startRow);

    // Widgets
    QLabel* banner;
    QLabel* loadingLabel;
    QLineEdit* urlInput;
    QPushButton* downBtn;
    QPushButton* queueBtn;
    QPushButton* streamBtn;
    QPushButton* processQueueBtn;
    QPushButton* streamQueueBtn;
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

    // Backends
    SgYtDlp* downloader;
    SgYtDlp* titleResolver;
    SgYtDlp* cdnPrefetcher;

    // State
    bool    isFetchingMetadata = false;
    bool    isProcessingQueue = false;
    QString cachedTitle;
    QString m_pendingPlaylistUrl;

    // Cache state: Maps raw URL to a pair of <VideoUrl, AudioUrl>
    QMap<QString, QPair<QUrl, QUrl>> cdnCache;
    QString m_currentlyPrefetchingUrl;

    // Background title resolution queue
    QList<QPair<int, QString>> m_titleQueue;
    int m_currentResolvingRow = -1;
};
#endif