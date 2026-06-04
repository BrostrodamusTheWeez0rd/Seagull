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
#include <QString>
#include <QPoint>
#include "../Backend/SgYtDlp.h"

class Downloads : public QWidget {
    Q_OBJECT
public:
    explicit Downloads(QWidget* parent = nullptr);

signals:
    void playMediaRequested(const QUrl& url, const QString& title);

private slots:
    void onDownloadClicked();
    void onAddToQueueClicked();
    void onProcessQueueClicked();
    void onStreamClicked();
    void onStreamQueueClicked();
    void onUrlTextChanged(const QString& text);
    void triggerMetadataFetch();
    void handleMetadataReady(const QString& title, const QString& uploader, const QString& duration, const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);
    void handleStreamUrlReady(const QUrl& directUrl);
    void handleLogMessage(const QString& message);
    void handleProgress(double percentage);
    void handleFinished(bool success);
    void showContextMenu(const QPoint& pos);
    void downloadSelectedItems();
    void removeSelectedItems();

private:
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
    QTimer* debounceTimer;
    SgYtDlp* downloader;
    bool wantsStreamPlayback = false;
    bool isFetchingMetadata = false;
    bool isProcessingQueue = false;
    QUrl cachedStreamUrl;
    QString cachedTitle;
};
#endif