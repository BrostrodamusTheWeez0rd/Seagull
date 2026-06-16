#include "SgMetaCache.h"
#include <QDateTime>

QJsonObject SgMetaCache::get(const QString& url) {
    const auto it = m_cache.constFind(url);
    if (it == m_cache.constEnd()) return {};
    if (QDateTime::currentMSecsSinceEpoch() - it->atMs > kTtlMs) {
        m_cache.remove(url);
        return {};
    }
    return it->obj;
}

void SgMetaCache::put(const QString& url, const QJsonObject& obj) {
    if (url.isEmpty() || obj["is_live"].toBool()) return; // live URLs rotate — never cache
    if (m_cache.size() > kMaxEntries) m_cache.clear();    // keep it bounded
    m_cache.insert(url, { obj, QDateTime::currentMSecsSinceEpoch() });
}

void SgMetaCache::evict(const QString& url) {
    m_cache.remove(url);
}
