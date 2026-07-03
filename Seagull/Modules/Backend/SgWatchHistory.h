#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

// Singleton that persists per-item playback progress to Config/watch_history.json,
// so the player can resume where the user left off and the Library can show a
// "Continue Watching" row. Mirrors SgFavorites' shape: a JSON store under Config/,
// rewritten on every change, loaded once on first access.
//
// Entries are keyed by a stable identifier:
//   - online media: the page URL (VideoPlayer::currentBaseUrl)
//   - local files:  the local file path
// Live streams and still photos are never recorded (no meaningful resume point).
class SgWatchHistory : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QString key;              // page URL (online) or file path (local)
        QString title;
        QString thumbUrl;         // network thumbnail URL for the Continue-Watching card (may be empty)
        QString site;             // "youtube" / "pornhub" / "chaturbate" / "local" / "other"
        qint64  positionMs = 0;
        qint64  durationMs = 0;
        qint64  updatedAt  = 0;   // ms since epoch — drives most-recent-first ordering
        bool    isLocal    = false;
        int     kind       = 0;   // matches MediaKind: 0=Video, 1=Audio (Photo never stored)
        bool    completed  = false; // watched to (near) the end — no resume, hidden from Continue Watching
    };

    static SgWatchHistory* instance();

    // The site bucket a key belongs to (drives the per-site home Continue Watching rows).
    // Home pages are site-specific, so YouTube must never surface local / PornHub items.
    static QString siteForKey(const QString& key, bool isLocal);

    // Record/update progress for an item. A near-complete position marks the entry
    // completed (resumePosition then returns -1 and it drops off Continue Watching).
    // No-op if key is empty or durationMs <= 0 (live / unknown length).
    void record(const QString& key, const QString& title, qint64 positionMs, qint64 durationMs,
                bool isLocal, int kind, const QString& thumbUrl = QString());

    // Saved resume position in ms, or -1 if none worth resuming (missing entry,
    // completed, under the minimum threshold, or within the end guard).
    qint64 resumePosition(const QString& key) const;

    bool  hasEntry(const QString& key) const;
    Entry entry(const QString& key) const;

    // Most-recent-first. continueWatching() excludes completed entries and any with
    // no meaningful resume point; recent() returns everything. The site overload keeps
    // each home feed to its own source (empty site == any).
    QList<Entry> continueWatching(int max = 50) const;
    QList<Entry> continueWatching(const QString& site, int max) const;
    QList<Entry> recent(int max = 100) const;

    // Cheap "is there anything to continue for this site?" check (no list building) —
    // used to decide whether the Continue Watching pill shows.
    bool hasResumable(const QString& site) const;

    void remove(const QString& key);
    void clearAll();

signals:
    void changed();

private:
    explicit SgWatchHistory(QObject* parent = nullptr);
    void load();
    void save() const;
    void trim(); // cap the store so it can't grow without bound

    bool resumable(const Entry& e) const; // shared gate for resumePosition + continueWatching

    // key -> entry; m_order holds keys most-recent-first (front = newest).
    QHash<QString, Entry> m_entries;
    QList<QString>        m_order;
};
