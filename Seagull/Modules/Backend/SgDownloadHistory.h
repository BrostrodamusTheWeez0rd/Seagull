#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

// Singleton that persists the download-manager's history to Config/download_history.json,
// so the Downloads tab survives restarts (and a download can be restarted from its stored
// page URL). Mirrors SgWatchHistory's shape: a JSON array under Config/, rewritten on every
// change, loaded once on first access.
//
// Records are keyed by the PAGE URL (never a CDN/stream URL — those expire fast). A restart
// simply re-runs yt-dlp against that page URL, which re-resolves a fresh CDN each time.
class SgDownloadHistory : public QObject {
    Q_OBJECT
public:
    // Lifecycle status of one download. Stored as an int in the JSON.
    enum Status { Queued = 0, Downloading = 1, Completed = 2, Failed = 3, Canceled = 4 };

    struct Record {
        QString pageUrl;          // the page/watch URL — the stable key, re-resolvable to CDN
        QString title;
        QString thumbUrl;         // network thumbnail URL for the row (may be empty)
        QString site;             // "youtube" / "pornhub" / "chaturbate" / "soundcloud" / "twitch" / "other"
        QString filePath;         // final saved file (parsed from yt-dlp output), empty until done
        int     status = Queued;
        qint64  addedAt = 0;      // ms since epoch — drives most-recent-first ordering
        qint64  finishedAt = 0;   // ms since epoch when it completed/failed (0 while pending)
    };

    static SgDownloadHistory* instance();

    // The site bucket a page URL belongs to (display only).
    static QString siteForUrl(const QString& url);

    // Create or update a record for this page URL, moving it to the front (most recent) and
    // setting its status to Queued. Preserves an existing thumbnail if none is supplied.
    void record(const QString& pageUrl, const QString& title, const QString& thumbUrl = QString());

    // Status transitions. setStatus stamps finishedAt for terminal states. setFilePath stores
    // the resolved output path (from yt-dlp's Destination/Merger lines) without reordering.
    void setStatus(const QString& pageUrl, int status);
    void setFilePath(const QString& pageUrl, const QString& filePath);

    bool   hasRecord(const QString& pageUrl) const;
    Record recordFor(const QString& pageUrl) const; // by-value copy; empty Record if missing

    // Most-recent-first snapshot for the UI to render.
    QList<Record> records() const;

    void remove(const QString& pageUrl);
    void clearAll();
    void clearFinished(); // drop Completed / Failed / Canceled; keep Queued / Downloading

signals:
    void changed();

private:
    explicit SgDownloadHistory(QObject* parent = nullptr);
    void load();
    void save() const;
    void trim(); // cap the store so it can't grow without bound

    // key (page URL) -> record; m_order holds keys most-recent-first (front = newest).
    QHash<QString, Record> m_records;
    QList<QString>         m_order;
};
