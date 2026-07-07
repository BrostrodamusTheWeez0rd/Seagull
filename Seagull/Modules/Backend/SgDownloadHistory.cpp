#include "SgDownloadHistory.h"
#include "SgPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

// Unique record id: creation timestamp + a session counter (two clicks inside the
// same millisecond stay distinct).
QString freshId() {
    static quint64 s_seq = 0;
    return QString::number(QDateTime::currentMSecsSinceEpoch())
         + QLatin1Char('-') + QString::number(++s_seq);
}

// Video/Audio bucket for a file extension — used to backfill kind on records
// from before kind/fmt were captured at enqueue time. "" when unrecognised.
QString kindForExt(const QString& ext) {
    static const QStringList audio = { "mp3", "m4a", "flac", "wav", "opus",
                                       "aac", "ogg", "oga", "wma" };
    static const QStringList video = { "mp4", "mkv", "webm", "mov", "avi",
                                       "m4v", "ts", "flv" };
    if (audio.contains(ext)) return QStringLiteral("Audio");
    if (video.contains(ext)) return QStringLiteral("Video");
    return QString();
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

QString SgDownloadHistory::add(const QString& pageUrl, const QString& title,
                               const QString& thumbUrl, const QString& kind,
                               const QString& fmt) {
    if (pageUrl.isEmpty()) return QString();

    Record r;
    r.id       = freshId();
    r.pageUrl  = pageUrl;
    r.title    = title;
    r.thumbUrl = thumbUrl;
    r.site     = siteForUrl(pageUrl);
    r.kind     = kind;
    r.fmt      = fmt;
    r.status   = Queued;
    r.addedAt  = QDateTime::currentMSecsSinceEpoch();

    m_records.insert(r.id, r);
    m_order.prepend(r.id); // most recent first
    trim();
    save();
    emit changed();
    return r.id;
}

void SgDownloadHistory::requeue(const QString& id) {
    const auto it = m_records.find(id);
    if (it == m_records.end()) return;
    it->status     = Queued;
    it->addedAt    = QDateTime::currentMSecsSinceEpoch();
    it->finishedAt = 0;
    it->filePath.clear(); // a fresh run resolves a new file
    m_order.removeOne(id);
    m_order.prepend(id);
    save();
    emit changed();
}

QString SgDownloadHistory::pendingDuplicate(const QString& pageUrl, const QString& kind,
                                            const QString& fmt) const {
    for (const Record& r : m_records) {
        if ((r.status == Queued || r.status == Downloading)
            && r.pageUrl == pageUrl && r.kind == kind && r.fmt == fmt)
            return r.id;
    }
    return QString();
}

void SgDownloadHistory::setStatus(const QString& id, int status) {
    const auto it = m_records.find(id);
    if (it == m_records.end()) return;
    if (it->status == status) return;
    it->status = status;
    it->finishedAt = isTerminal(status) ? QDateTime::currentMSecsSinceEpoch() : 0;
    save();
    emit changed();
}

void SgDownloadHistory::setFilePath(const QString& id, const QString& filePath) {
    const auto it = m_records.find(id);
    if (it == m_records.end() || filePath.isEmpty() || it->filePath == filePath) return;
    it->filePath = filePath;
    save();
    // No emit changed(): the path lands mid-download; the row's live update covers display.
}

SgDownloadHistory::Record SgDownloadHistory::recordFor(const QString& id) const {
    return m_records.value(id);
}

QList<SgDownloadHistory::Record> SgDownloadHistory::records() const {
    QList<Record> out;
    out.reserve(m_order.size());
    for (const QString& k : m_order) out.append(m_records[k]);
    return out;
}

void SgDownloadHistory::remove(const QString& id) {
    if (!m_records.contains(id)) return;
    m_records.remove(id);
    m_order.removeOne(id);
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
        r.id         = obj.value("id").toString();
        r.pageUrl    = obj.value("pageUrl").toString();
        r.title      = obj.value("title").toString();
        r.thumbUrl   = obj.value("thumbUrl").toString();
        r.site       = obj.value("site").toString();
        r.kind       = obj.value("kind").toString();
        r.fmt        = obj.value("fmt").toString();
        r.filePath   = obj.value("filePath").toString();
        r.status     = obj.value("status").toInt();
        r.addedAt    = static_cast<qint64>(obj.value("addedAt").toDouble());
        r.finishedAt = static_cast<qint64>(obj.value("finishedAt").toDouble());
        // A record left mid-flight (app killed while downloading/queued) can't still be
        // running, so present it as Failed — restartable from its page URL.
        if (r.status == Downloading || r.status == Queued) r.status = Failed;
        if (r.site.isEmpty()) r.site = siteForUrl(r.pageUrl); // backfill older entries
        if (r.id.isEmpty()) r.id = freshId(); // entries from before id-keying
        // Entries from before kind/fmt were captured: derive both from the saved
        // file's extension so old users' rows show a type too. No file recorded
        // (e.g. an old failed download) means there's nothing to derive — the
        // row's type label stays hidden for those.
        if (r.kind.isEmpty() && !r.filePath.isEmpty()) {
            const QString ext = QFileInfo(r.filePath).suffix().toLower();
            r.kind = kindForExt(ext);
            if (r.fmt.isEmpty() && !ext.isEmpty()) r.fmt = ext;
        }
        if (r.pageUrl.isEmpty() || m_records.contains(r.id)) continue;
        m_records.insert(r.id, r);
        m_order.append(r.id);
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
        obj["id"]         = r.id;
        obj["pageUrl"]    = r.pageUrl;
        obj["title"]      = r.title;
        obj["thumbUrl"]   = r.thumbUrl;
        obj["site"]       = r.site;
        obj["kind"]       = r.kind;
        obj["fmt"]        = r.fmt;
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
