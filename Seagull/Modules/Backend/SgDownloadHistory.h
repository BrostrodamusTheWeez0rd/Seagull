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
// Records are keyed by a UNIQUE ID, not the page URL — the same link downloaded twice
// (e.g. once as video, once as audio) is two separate rows. The page URL is stored per
// record (never a CDN/stream URL — those expire fast): a restart re-runs yt-dlp against
// it, which re-resolves a fresh CDN each time.
class SgDownloadHistory : public QObject {
    Q_OBJECT
public:
    // Lifecycle status of one download. Stored as an int in the JSON.
    enum Status { Queued = 0, Downloading = 1, Completed = 2, Failed = 3, Canceled = 4 };

    struct Record {
        QString id;               // unique key (stamped at creation; survives restarts)
        QString pageUrl;          // the page/watch URL — re-resolvable to a fresh CDN
        QString title;
        QString thumbUrl;         // network thumbnail URL for the row (may be empty)
        QString site;             // "youtube" / "pornhub" / "chaturbate" / "soundcloud" / "twitch" / "other"
        QString kind;             // Download/Type at enqueue time: "Video" / "Audio"
        QString fmt;              // Download/Format at enqueue time (e.g. "mp4", "Best Available")
        QString filePath;         // final saved file (parsed from yt-dlp output), empty until done
        int     status = Queued;
        qint64  addedAt = 0;      // ms since epoch — drives most-recent-first ordering
        qint64  finishedAt = 0;   // ms since epoch when it completed/failed (0 while pending)
    };

    static SgDownloadHistory* instance();

    // The site bucket a page URL belongs to (display only).
    static QString siteForUrl(const QString& url);

    // Create a NEW Queued record at the front (most recent) and return its id.
    QString add(const QString& pageUrl, const QString& title, const QString& thumbUrl,
                const QString& kind, const QString& fmt);

    // Re-queue an existing record (Restart): status back to Queued, fresh addedAt,
    // stale file path dropped, moved to the front.
    void requeue(const QString& id);

    // The id of a Queued/Downloading record for this exact url+kind+fmt combination
    // ("" if none) — lets the manager ignore a duplicate click while one is pending.
    QString pendingDuplicate(const QString& pageUrl, const QString& kind, const QString& fmt) const;

    // Status transitions. setStatus stamps finishedAt for terminal states. setFilePath stores
    // the resolved output path (from yt-dlp's Destination/Merger lines) without reordering.
    void setStatus(const QString& id, int status);
    void setFilePath(const QString& id, const QString& filePath);

    Record recordFor(const QString& id) const; // by-value copy; empty Record if missing

    // Most-recent-first snapshot for the UI to render.
    QList<Record> records() const;

    void remove(const QString& id);
    void clearAll();
    void clearFinished(); // drop Completed / Failed / Canceled; keep Queued / Downloading

signals:
    void changed();

private:
    explicit SgDownloadHistory(QObject* parent = nullptr);
    void load();
    void save() const;
    void trim(); // cap the store so it can't grow without bound

    // key (unique id) -> record; m_order holds ids most-recent-first (front = newest).
    QHash<QString, Record> m_records;
    QList<QString>         m_order;
};
