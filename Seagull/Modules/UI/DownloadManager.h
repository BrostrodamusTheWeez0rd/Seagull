#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QUrl>

class SgYtDlp;
class DownloadRow;
class QVBoxLayout;
class QLabel;
class QNetworkAccessManager;

// The Download Manager tab: a persistent, clearable list of downloads with live per-row
// progress and status, restart, and cancel. It owns the ad-hoc download FIFO (moved out of
// the orchestrator) and drives the dedicated download worker sequentially — one at a time,
// matching the app's avoidance of yt-dlp process contention. Records persist via
// SgDownloadHistory, keyed by a unique per-download id (the same page URL downloaded as
// video and again as audio is two rows); a restart re-runs the record's stored page URL,
// which re-resolves a fresh CDN.
class DownloadManager : public QWidget {
    Q_OBJECT
public:
    explicit DownloadManager(SgYtDlp* downloadWorker, QWidget* parent = nullptr);

public slots:
    // Queue a download from its page URL (+ display metadata). Records it, then pumps.
    void enqueue(const QUrl& pageUrl, const QString& title, const QString& thumbUrl);

signals:
    // busy=true while a download runs (percent 0-100, or <0 when indeterminate); busy=false
    // when the queue drains. Drives the Downloads tab-header progress indicator.
    void activity(bool busy, double percent);
    // Reveal the finished file in the OS file browser.
    void openFileRequested(const QString& filePath);

private:
    void pump();          // start the next queued download if idle
    void rebuild();       // rebuild the row list from SgDownloadHistory (structural changes)

    // Worker signal handlers.
    void onProgress(double percent, const QString& speed, const QString& eta);
    void onDestination(const QString& path);
    void onFinished(bool ok);

    // Row action handlers (record ids).
    void restart(const QString& id);
    void cancel(const QString& id);
    void removeOne(const QString& id);

    SgYtDlp*               m_worker = nullptr;
    QNetworkAccessManager* m_nam = nullptr;

    QStringList m_queue;          // record ids pending; the active one stays at front while running
    bool        m_downloading = false;
    QString     m_activeKey;      // record id of the in-flight download ("" = none)
    bool        m_canceling = false; // the active download is being user-canceled (mark Canceled)

    QVBoxLayout* m_listLayout = nullptr; // holds the DownloadRow widgets + the empty-state label
    QLabel*      m_emptyLabel = nullptr;
    QHash<QString, DownloadRow*> m_rows; // record id -> live row (rebuilt each structural change)
};

#endif // DOWNLOADMANAGER_H
