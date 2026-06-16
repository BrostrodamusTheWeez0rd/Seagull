#pragma once
#include <QObject>
#include <QHash>
#include <QString>
#include <QJsonObject>

// One shared yt-dlp `-J` metadata cache, owned by the orchestrator and handed to
// every SgYtDlp worker (the same injection pattern as the HLS proxy). Without it
// each worker caches in isolation, so the queue title-resolver, the prefetcher,
// and the player each re-extract the SAME video — three near-simultaneous hits at
// YouTube, which is exactly what trips its bot/throttle checks. Sharing collapses
// those to one extraction; the later callers answer from the cache.
//
// GUI-thread only (all workers are parented to the orchestrator, none are moved to
// a thread), so no locking is needed. Live streams are never cached (their CDN URLs
// rotate); a stale-URL re-resolve evicts its entry first.
class SgMetaCache : public QObject {
    Q_OBJECT
public:
    explicit SgMetaCache(QObject* parent = nullptr) : QObject(parent) {}

    // Fresh entry for this URL, or an empty object if absent/expired (expired
    // entries are dropped on access).
    QJsonObject get(const QString& url);
    // Store the -J result. No-ops for empty URLs and live streams.
    void put(const QString& url, const QJsonObject& obj);
    // Drop one entry (used when a cached stream URL went stale and must be refetched).
    void evict(const QString& url);

private:
    struct Entry { QJsonObject obj; qint64 atMs; };
    QHash<QString, Entry> m_cache;

    static constexpr qint64 kTtlMs = 30 * 60 * 1000; // CDN URLs comfortably outlive this
    static constexpr int    kMaxEntries = 64;        // bounded working set
};
