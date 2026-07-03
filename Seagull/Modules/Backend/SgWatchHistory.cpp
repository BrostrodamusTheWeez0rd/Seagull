#include "SgWatchHistory.h"
#include "SgPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// Don't offer to resume the first few seconds (feels like a bug, not a resume).
constexpr qint64 kMinResumeMs   = 10'000;
// Positions within this of the end count as "finished" — start over next time.
constexpr qint64 kEndGuardMs    = 15'000;
// Or past this fraction of the total (covers short clips where the guard is a big slice).
constexpr double kCompleteFrac  = 0.95;
// Hard cap on stored entries; oldest fall off the tail.
constexpr int    kMaxEntries    = 200;

const char* kStoreFile = "watch_history.json";
}

SgWatchHistory* SgWatchHistory::instance() {
    static SgWatchHistory* s_instance = new SgWatchHistory(nullptr);
    return s_instance;
}

SgWatchHistory::SgWatchHistory(QObject* parent) : QObject(parent) {
    load();
}

QString SgWatchHistory::siteForKey(const QString& key, bool isLocal) {
    if (isLocal) return QStringLiteral("local");
    if (key.contains("youtube.com", Qt::CaseInsensitive) ||
        key.contains("youtu.be",   Qt::CaseInsensitive)) return QStringLiteral("youtube");
    if (key.contains("pornhub.com",   Qt::CaseInsensitive)) return QStringLiteral("pornhub");
    if (key.contains("chaturbate.com", Qt::CaseInsensitive)) return QStringLiteral("chaturbate");
    if (key.contains("soundcloud.com", Qt::CaseInsensitive)) return QStringLiteral("soundcloud");
    // Live streams never record, but the bucket keeps any twitch.tv key out of "other".
    if (key.contains("twitch.tv",     Qt::CaseInsensitive)) return QStringLiteral("twitch");
    return QStringLiteral("other");
}

bool SgWatchHistory::resumable(const Entry& e) const {
    if (e.completed) return false;
    if (e.positionMs < kMinResumeMs) return false;
    if (e.durationMs > 0 && e.positionMs >= e.durationMs - kEndGuardMs) return false;
    return true;
}

void SgWatchHistory::record(const QString& key, const QString& title, qint64 positionMs,
                            qint64 durationMs, bool isLocal, int kind, const QString& thumbUrl) {
    if (key.isEmpty() || durationMs <= 0) return; // live / unknown length — nothing to resume to

    Entry e = m_entries.value(key); // existing entry (to preserve thumb) or a default
    e.key        = key;
    e.title      = title;
    e.site       = siteForKey(key, isLocal);
    if (!thumbUrl.isEmpty()) e.thumbUrl = thumbUrl; // keep the prior thumbnail if none supplied
    e.positionMs = positionMs;
    e.durationMs = durationMs;
    e.isLocal    = isLocal;
    e.kind       = kind;
    e.updatedAt  = QDateTime::currentMSecsSinceEpoch();
    e.completed  = positionMs >= durationMs - kEndGuardMs
                   || positionMs >= qint64(durationMs * kCompleteFrac);

    m_entries.insert(key, e);
    m_order.removeOne(key);
    m_order.prepend(key); // most recent first
    trim();
    save();
    emit changed();
}

qint64 SgWatchHistory::resumePosition(const QString& key) const {
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd()) return -1;
    return resumable(it.value()) ? it.value().positionMs : -1;
}

bool SgWatchHistory::hasEntry(const QString& key) const {
    return m_entries.contains(key);
}

SgWatchHistory::Entry SgWatchHistory::entry(const QString& key) const {
    return m_entries.value(key);
}

QList<SgWatchHistory::Entry> SgWatchHistory::continueWatching(int max) const {
    return continueWatching(QString(), max);
}

QList<SgWatchHistory::Entry> SgWatchHistory::continueWatching(const QString& site, int max) const {
    QList<Entry> out;
    for (const QString& k : m_order) {
        const Entry& e = m_entries[k];
        if (!resumable(e)) continue;
        if (!site.isEmpty() && e.site != site) continue; // keep each home feed to its own source
        out.append(e);
        if (out.size() >= max) break;
    }
    return out;
}

bool SgWatchHistory::hasResumable(const QString& site) const {
    for (const QString& k : m_order) {
        const Entry& e = m_entries[k];
        if (!resumable(e)) continue;
        if (!site.isEmpty() && e.site != site) continue;
        return true;
    }
    return false;
}

QList<SgWatchHistory::Entry> SgWatchHistory::recent(int max) const {
    QList<Entry> out;
    for (const QString& k : m_order) {
        out.append(m_entries[k]);
        if (out.size() >= max) break;
    }
    return out;
}

void SgWatchHistory::remove(const QString& key) {
    if (!m_entries.contains(key)) return;
    m_entries.remove(key);
    m_order.removeOne(key);
    save();
    emit changed();
}

void SgWatchHistory::clearAll() {
    if (m_entries.isEmpty() && m_order.isEmpty()) return;
    m_entries.clear();
    m_order.clear();
    save();
    emit changed();
}

void SgWatchHistory::trim() {
    while (m_order.size() > kMaxEntries) {
        const QString dropped = m_order.takeLast();
        m_entries.remove(dropped);
    }
}

void SgWatchHistory::load() {
    const QString path = SgPaths::configDir() + "/" + QString::fromLatin1(kStoreFile);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return; // no file yet — start empty

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    // Stored newest-first; preserve that order into m_order.
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject obj = v.toObject();
        Entry e;
        e.key        = obj.value("key").toString();
        e.title      = obj.value("title").toString();
        e.thumbUrl   = obj.value("thumbUrl").toString();
        e.site       = obj.value("site").toString();
        e.positionMs = static_cast<qint64>(obj.value("positionMs").toDouble());
        e.durationMs = static_cast<qint64>(obj.value("durationMs").toDouble());
        e.updatedAt  = static_cast<qint64>(obj.value("updatedAt").toDouble());
        e.isLocal    = obj.value("isLocal").toBool();
        e.kind       = obj.value("kind").toInt();
        e.completed  = obj.value("completed").toBool();
        if (e.site.isEmpty()) e.site = siteForKey(e.key, e.isLocal); // backfill older entries
        if (e.key.isEmpty() || m_entries.contains(e.key)) continue;
        m_entries.insert(e.key, e);
        m_order.append(e.key);
    }
    trim();
}

void SgWatchHistory::save() const {
    const QString dir  = SgPaths::configDir();
    const QString path = dir + "/" + QString::fromLatin1(kStoreFile);

    QDir().mkpath(dir); // ensure Config/ exists (matches SgFavorites)

    QJsonArray arr;
    for (const QString& k : m_order) {
        const Entry& e = m_entries[k];
        QJsonObject obj;
        obj["key"]        = e.key;
        obj["title"]      = e.title;
        obj["thumbUrl"]   = e.thumbUrl;
        obj["site"]       = e.site;
        obj["positionMs"] = static_cast<double>(e.positionMs);
        obj["durationMs"] = static_cast<double>(e.durationMs);
        obj["updatedAt"]  = static_cast<double>(e.updatedAt);
        obj["isLocal"]    = e.isLocal;
        obj["kind"]       = e.kind;
        obj["completed"]  = e.completed;
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}
