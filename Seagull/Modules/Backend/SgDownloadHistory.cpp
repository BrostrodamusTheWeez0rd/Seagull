#include "SgDownloadHistory.h"
#include "SgPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// Hard cap on stored records; oldest fall off the tail.
constexpr int kMaxRecords = 200;
const char* kStoreFile = "download_history.json";

bool isTerminal(int status) {
    return status == SgDownloadHistory::Completed
        || status == SgDownloadHistory::Failed
        || status == SgDownloadHistory::Canceled;
}
}

SgDownloadHistory* SgDownloadHistory::instance() {
    static SgDownloadHistory* s_instance = new SgDownloadHistory(nullptr);
    return s_instance;
}

SgDownloadHistory::SgDownloadHistory(QObject* parent) : QObject(parent) {
    load();
}

QString SgDownloadHistory::siteForUrl(const QString& url) {
    if (url.contains("youtube.com", Qt::CaseInsensitive) ||
        url.contains("youtu.be",   Qt::CaseInsensitive)) return QStringLiteral("youtube");
    if (url.contains("pornhub.com",    Qt::CaseInsensitive)) return QStringLiteral("pornhub");
    if (url.contains("chaturbate.com", Qt::CaseInsensitive)) return QStringLiteral("chaturbate");
    if (url.contains("soundcloud.com", Qt::CaseInsensitive)) return QStringLiteral("soundcloud");
    if (url.contains("twitch.tv",      Qt::CaseInsensitive)) return QStringLiteral("twitch");
    return QStringLiteral("other");
}

void SgDownloadHistory::record(const QString& pageUrl, const QString& title,
                               const QString& thumbUrl) {
    if (pageUrl.isEmpty()) return;

    Record r = m_records.value(pageUrl); // existing (to preserve thumb/path) or a default
    r.pageUrl    = pageUrl;
    if (!title.isEmpty())    r.title    = title;
    if (!thumbUrl.isEmpty()) r.thumbUrl = thumbUrl; // keep the prior thumbnail if none supplied
    r.site       = siteForUrl(pageUrl);
    r.status     = Queued;
    r.addedAt    = QDateTime::currentMSecsSinceEpoch();
    r.finishedAt = 0;
    r.filePath.clear(); // a fresh run resolves a new file

    m_records.insert(pageUrl, r);
    m_order.removeOne(pageUrl);
    m_order.prepend(pageUrl); // most recent first
    trim();
    save();
    emit changed();
}

void SgDownloadHistory::setStatus(const QString& pageUrl, int status) {
    const auto it = m_records.find(pageUrl);
    if (it == m_records.end()) return;
    if (it->status == status) return;
    it->status = status;
    it->finishedAt = isTerminal(status) ? QDateTime::currentMSecsSinceEpoch() : 0;
    save();
    emit changed();
}

void SgDownloadHistory::setFilePath(const QString& pageUrl, const QString& filePath) {
    const auto it = m_records.find(pageUrl);
    if (it == m_records.end() || filePath.isEmpty() || it->filePath == filePath) return;
    it->filePath = filePath;
    save();
    // No emit changed(): the path lands mid-download; the row's live update covers display.
}

bool SgDownloadHistory::hasRecord(const QString& pageUrl) const {
    return m_records.contains(pageUrl);
}

SgDownloadHistory::Record SgDownloadHistory::recordFor(const QString& pageUrl) const {
    return m_records.value(pageUrl);
}

QList<SgDownloadHistory::Record> SgDownloadHistory::records() const {
    QList<Record> out;
    out.reserve(m_order.size());
    for (const QString& k : m_order) out.append(m_records[k]);
    return out;
}

void SgDownloadHistory::remove(const QString& pageUrl) {
    if (!m_records.contains(pageUrl)) return;
    m_records.remove(pageUrl);
    m_order.removeOne(pageUrl);
    save();
    emit changed();
}

void SgDownloadHistory::clearAll() {
    if (m_records.isEmpty() && m_order.isEmpty()) return;
    m_records.clear();
    m_order.clear();
    save();
    emit changed();
}

void SgDownloadHistory::clearFinished() {
    bool any = false;
    for (int i = m_order.size() - 1; i >= 0; --i) {
        const QString& k = m_order[i];
        if (isTerminal(m_records.value(k).status)) {
            m_records.remove(k);
            m_order.removeAt(i);
            any = true;
        }
    }
    if (!any) return;
    save();
    emit changed();
}

void SgDownloadHistory::trim() {
    while (m_order.size() > kMaxRecords) {
        const QString dropped = m_order.takeLast();
        m_records.remove(dropped);
    }
}

void SgDownloadHistory::load() {
    const QString path = SgPaths::configDir() + "/" + QString::fromLatin1(kStoreFile);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return; // no file yet — start empty

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    // Stored newest-first; preserve that order into m_order.
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject obj = v.toObject();
        Record r;
        r.pageUrl    = obj.value("pageUrl").toString();
        r.title      = obj.value("title").toString();
        r.thumbUrl   = obj.value("thumbUrl").toString();
        r.site       = obj.value("site").toString();
        r.filePath   = obj.value("filePath").toString();
        r.status     = obj.value("status").toInt();
        r.addedAt    = static_cast<qint64>(obj.value("addedAt").toDouble());
        r.finishedAt = static_cast<qint64>(obj.value("finishedAt").toDouble());
        // A record left mid-flight (app killed while downloading/queued) can't still be
        // running, so present it as Failed — restartable from its page URL.
        if (r.status == Downloading || r.status == Queued) r.status = Failed;
        if (r.site.isEmpty()) r.site = siteForUrl(r.pageUrl); // backfill older entries
        if (r.pageUrl.isEmpty() || m_records.contains(r.pageUrl)) continue;
        m_records.insert(r.pageUrl, r);
        m_order.append(r.pageUrl);
    }
    trim();
}

void SgDownloadHistory::save() const {
    const QString dir  = SgPaths::configDir();
    const QString path = dir + "/" + QString::fromLatin1(kStoreFile);

    QDir().mkpath(dir); // ensure Config/ exists (matches SgFavorites / SgWatchHistory)

    QJsonArray arr;
    for (const QString& k : m_order) {
        const Record& r = m_records[k];
        QJsonObject obj;
        obj["pageUrl"]    = r.pageUrl;
        obj["title"]      = r.title;
        obj["thumbUrl"]   = r.thumbUrl;
        obj["site"]       = r.site;
        obj["filePath"]   = r.filePath;
        obj["status"]     = r.status;
        obj["addedAt"]    = static_cast<double>(r.addedAt);
        obj["finishedAt"] = static_cast<double>(r.finishedAt);
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}
