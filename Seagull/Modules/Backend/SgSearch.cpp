#include "SgSearch.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

SgSearch::SgSearch(QObject* parent) : QObject(parent) {
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, &SgSearch::handleFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit failed("Could not find yt-dlp.exe.");
        });
}

SgSearch::~SgSearch() {
    cancel();
}

void SgSearch::search(Site site, const QString& query, int limit, bool shortsOnly) {
    if (query.trimmed().isEmpty()) { emit failed("Empty search query."); return; }

    cancel(); // drop any in-flight query (a new search supersedes it; no 'failed' emitted)

    m_site = site;
    m_mode = Mode::VideoSearch;
    m_buffer.clear();

    if (shortsOnly && site == Site::YouTube) {
        startShortsSearch(query.trimmed(), limit);
        return;
    }

    const QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings";

    // Per-site search target. New sites slot in here with their own branch.
    switch (site) {
    case Site::YouTube:
        args << QString("ytsearch%1:%2").arg(limit).arg(query.trimmed());
        break;
    }

    emit logMessage("Searching: " + query.trimmed());
    m_process->start(exePath, args);
}

void SgSearch::fetchChannelVideos(const QString& channelUrl, int limit) {
    if (channelUrl.trimmed().isEmpty()) { emit failed("No channel to open."); return; }

    cancel(); // a channel open supersedes any in-flight query

    m_site = Site::YouTube;
    m_mode = Mode::ChannelList;
    m_buffer.clear();

    const QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    // The channel's uploads tab. Strip a trailing slash, then target /videos
    // (so a bare channel/handle URL doesn't list every tab as nested playlists).
    QString url = channelUrl.trimmed();
    while (url.endsWith('/')) url.chop(1);
    if (!url.endsWith("/videos")) url += "/videos";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings"
         << "--playlist-end" << QString::number(qMax(1, limit))
         << url;

    emit logMessage("Opening channel: " + url);
    m_process->start(exePath, args);
}

void SgSearch::cancel() {
    if (m_process->state() == QProcess::Running) {
        m_cancelled = true; // tells handleFinished to ignore this killed run
        m_process->kill();
        m_process->waitForFinished();
    }
    if (m_shortsReply) {
        m_shortsReply->disconnect(this); // no finished handler for an aborted page
        m_shortsReply->abort();
        m_shortsReply->deleteLater();
        m_shortsReply = nullptr;
    }
    m_buffer.clear();
}

void SgSearch::handleFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (m_cancelled) { m_cancelled = false; return; } // superseded by a newer query

    m_buffer.append(m_process->readAllStandardOutput());

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        const QString err = QString::fromLocal8Bit(m_buffer).trimmed();
        emit failed(err.isEmpty() ? "Search failed." : ("Search failed: " + err.right(400)));
        return;
    }

    const int jsonStart = m_buffer.indexOf('{');
    if (jsonStart == -1) { emit failed("No results."); return; }

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(m_buffer.mid(jsonStart), &perr);
    if (perr.error != QJsonParseError::NoError || doc.isNull()) {
        emit failed("Could not parse search results.");
        return;
    }

    if (m_mode == Mode::ChannelList) {
        SearchResult info;
        QList<SearchResult> videos;
        parseChannelList(doc.object(), info, videos);
        emit channelVideosReady(info, videos);
        return;
    }

    QList<SearchResult> results;
    switch (m_site) {
    case Site::YouTube:
        results = parseYoutube(doc.object());
        break;
    }
    emit resultsReady(results);
}

namespace {
// Pick a channel avatar from a yt-dlp thumbnails[] array. Prefer a near-square
// image (the avatar) over a wide one (the channel banner); among candidates, the
// largest. Falls back to the first url if none carry dimensions.
QString pickAvatar(const QJsonArray& thumbs) {
    QString url;
    int bestScore = -1;
    for (const auto& t : thumbs) {
        const QJsonObject to = t.toObject();
        const QString u = to["url"].toString();
        if (u.isEmpty()) continue;
        const int w = to["width"].toInt();
        const int h = to["height"].toInt();
        if (w <= 0 || h <= 0) { if (url.isEmpty()) url = u; continue; }
        const double ar = double(w) / double(h);
        const bool square = ar > 0.7 && ar < 1.4;        // avatar, not banner
        const int score = (square ? 1000000 : 0) + w;    // square wins, then size
        if (score > bestScore) { bestScore = score; url = u; }
    }
    return url;
}
}

bool SgSearch::parseVideoEntry(const QJsonObject& e, SearchResult& r) const {
    // Flat search entries put the watch URL in "url" (webpage_url is empty).
    const QString url = e["url"].toString();
    const QString title = e["title"].toString();
    if (url.isEmpty() || title.isEmpty()) return false;

    r.title = title;
    r.url = url;
    r.channel = e["channel"].toString();
    if (r.channel.isEmpty()) r.channel = e["uploader"].toString();
    // The uploader's channel page, so the card's channel name is clickable.
    r.channelUrl = e["channel_url"].toString();
    if (r.channelUrl.isEmpty()) r.channelUrl = e["uploader_url"].toString();

    // YouTube's thumbnails[] urls are .jpg-named but actually serve WebP (the
    // signed sqp/rs variants), which QPixmap can't decode without the WebP plugin.
    // The canonical /vi/<id>/mqdefault.jpg is a real 16:9 JPEG that always exists
    // and loads fine, so build that from the video id instead.
    const QString id = e["id"].toString();
    if (!id.isEmpty())
        r.thumbnail = QString("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id);
    else {
        int bestW = -1;
        for (const auto& t : e["thumbnails"].toArray()) {
            const QJsonObject to = t.toObject();
            const int w = to["width"].toInt();
            if (w > bestW) { bestW = w; r.thumbnail = to["url"].toString(); }
        }
    }

    r.duration = e.contains("duration") && !e["duration"].isNull()
        ? static_cast<qint64>(e["duration"].toDouble()) : -1;
    r.viewCount = e.contains("view_count") && !e["view_count"].isNull()
        ? static_cast<qint64>(e["view_count"].toDouble()) : -1;

    // Shorts rarely surface in normal yt-dlp search (the shorts shelf is skipped),
    // but tag any that do by their URL so the filter sees them.
    r.isShort = r.url.contains("/shorts/", Qt::CaseInsensitive);
    return true;
}

QList<SearchResult> SgSearch::parseYoutube(const QJsonObject& root) const {
    QList<SearchResult> out;
    const QJsonArray entries = root["entries"].toArray();

    for (const auto& it : entries) {
        const QJsonObject e = it.toObject();
        const QString ieKey = e["ie_key"].toString();

        // Channel results (ie_key "YoutubeTab") were dropped before; surface them
        // as channel cards now. Playlists are also "YoutubeTab" but carry a list
        // URL — still skip those.
        if (ieKey == "YoutubeTab") {
            const QString url = e["url"].toString();
            if (url.isEmpty() || url.contains("list=")) continue; // playlist/other tab
            SearchResult r;
            r.isChannel = true;
            r.title = e["title"].toString();
            if (r.title.isEmpty()) r.title = e["channel"].toString();
            if (r.title.isEmpty()) continue;
            r.channel    = r.title;
            r.url        = url;
            r.channelUrl = url;
            r.thumbnail  = pickAvatar(e["thumbnails"].toArray());
            r.subscriberCount =
                e.contains("channel_follower_count") && !e["channel_follower_count"].isNull()
                    ? static_cast<qint64>(e["channel_follower_count"].toDouble()) : -1;
            out.append(r);
            continue;
        }
        // Any other non-video kind — skip. Videos use ie_key "Youtube".
        if (!ieKey.isEmpty() && ieKey != "Youtube") continue;

        SearchResult r;
        if (parseVideoEntry(e, r)) out.append(r);
    }
    return out;
}

void SgSearch::parseChannelList(const QJsonObject& root, SearchResult& info,
                                QList<SearchResult>& videos) const {
    info.isChannel = true;
    info.title = root["channel"].toString();
    if (info.title.isEmpty()) info.title = root["uploader"].toString();
    if (info.title.isEmpty()) info.title = root["title"].toString();
    info.channel = info.title;
    info.url = root["channel_url"].toString();
    if (info.url.isEmpty()) info.url = root["uploader_url"].toString();
    if (info.url.isEmpty()) info.url = root["webpage_url"].toString();
    info.channelUrl = info.url;
    info.thumbnail = pickAvatar(root["thumbnails"].toArray());
    info.subscriberCount =
        root.contains("channel_follower_count") && !root["channel_follower_count"].isNull()
            ? static_cast<qint64>(root["channel_follower_count"].toDouble()) : -1;

    for (const auto& it : root["entries"].toArray()) {
        const QJsonObject e = it.toObject();
        // A channel root can come back as tabs/playlists nesting the videos one
        // level down — flatten that. (Targeting /videos usually avoids it.)
        if (e.contains("entries")) {
            for (const auto& it2 : e["entries"].toArray()) {
                SearchResult r;
                if (parseVideoEntry(it2.toObject(), r)) videos.append(r);
            }
            continue;
        }
        SearchResult r;
        if (parseVideoEntry(e, r)) videos.append(r);
    }
}

// ---------------------------------------------------------------------------
// Shorts search — YouTube internal search API
// ---------------------------------------------------------------------------

void SgSearch::startShortsSearch(const QString& query, int limit) {
    if (!m_nam) m_nam = new QNetworkAccessManager(this);

    m_shortsLimit = limit;
    if (query != m_shortsQuery) {
        m_shortsQuery = query;
        m_shortsResults.clear();
        m_shortsSeenIds.clear();
        m_shortsContinuation.clear();
        m_shortsExhausted = false;
    }

    // Already have enough pages cached (or no more exist) — answer immediately.
    if (m_shortsResults.size() >= limit || m_shortsExhausted) {
        emit resultsReady(m_shortsResults);
        return;
    }

    emit logMessage("Searching Shorts: " + query);
    fetchShortsPage();
}

void SgSearch::fetchShortsPage() {
    QJsonObject client{ {"clientName", "WEB"}, {"clientVersion", "2.20250613.00.00"} };
    QJsonObject body{ {"context", QJsonObject{ {"client", client} }} };
    if (m_shortsContinuation.isEmpty()) {
        body["query"]  = m_shortsQuery;
        body["params"] = "EgIQCQ=="; // YouTube search filter: type = Shorts
    } else {
        body["continuation"] = m_shortsContinuation;
    }

    QNetworkRequest req(QUrl("https://www.youtube.com/youtubei/v1/search?prettyPrint=false"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    m_shortsReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_shortsReply, &QNetworkReply::finished, this, &SgSearch::handleShortsReply);
}

void SgSearch::handleShortsReply() {
    QNetworkReply* reply = m_shortsReply;
    m_shortsReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        if (!m_shortsResults.isEmpty()) emit resultsReady(m_shortsResults); // keep what we have
        else emit failed("Shorts search failed: " + reply->errorString());
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (doc.isNull()) { emit failed("Could not parse Shorts results."); return; }

    QList<QJsonObject> lockups;
    collectObjects(doc.object(), "shortsLockupViewModel", lockups);

    const int before = m_shortsResults.size();
    for (const QJsonObject& l : lockups) {
        // Video id from the tap-through URL ("/shorts/<id>"), falling back to
        // the entity id ("shorts-shelf-item-<id>").
        QString id;
        const QString tapUrl = l["onTap"]["innertubeCommand"]["commandMetadata"]
                                ["webCommandMetadata"]["url"].toString();
        if (tapUrl.startsWith("/shorts/"))
            id = tapUrl.mid(QStringLiteral("/shorts/").size());
        else if (const QString entity = l["entityId"].toString();
                 entity.startsWith("shorts-shelf-item-"))
            id = entity.mid(QStringLiteral("shorts-shelf-item-").size());
        if (id.isEmpty() || m_shortsSeenIds.contains(id)) continue;

        QString title = l["overlayMetadata"]["primaryText"]["content"].toString();
        if (title.isEmpty()) title = l["accessibilityText"].toString();
        if (title.isEmpty()) continue;

        SearchResult r;
        r.title     = title;
        r.url       = "https://www.youtube.com/shorts/" + id;
        r.thumbnail = QString("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id);
        r.isShort   = true;
        m_shortsSeenIds.insert(id);
        m_shortsResults.append(r);
    }

    m_shortsContinuation.clear();
    QList<QJsonObject> conts;
    collectObjects(doc.object(), "continuationItemRenderer", conts);
    for (const QJsonObject& c : conts) {
        const QString tok =
            c["continuationEndpoint"]["continuationCommand"]["token"].toString();
        if (!tok.isEmpty()) { m_shortsContinuation = tok; break; }
    }

    // No token, or a page that contributed nothing new (loop guard) — done.
    if (m_shortsContinuation.isEmpty() || m_shortsResults.size() == before)
        m_shortsExhausted = true;

    if (m_shortsResults.size() < m_shortsLimit && !m_shortsExhausted)
        fetchShortsPage();
    else
        emit resultsReady(m_shortsResults);
}

// Depth-first collect of every object stored under `key` anywhere in the tree.
// YouTube's response nesting varies per layout, so structural paths can't be
// relied on; matching on the renderer name is the stable contract.
void SgSearch::collectObjects(const QJsonValue& v, const QString& key,
                              QList<QJsonObject>& out) {
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == key && it.value().isObject())
                out.append(it.value().toObject());
            collectObjects(it.value(), key, out);
        }
    } else if (v.isArray()) {
        for (const auto& e : v.toArray())
            collectObjects(e, key, out);
    }
}
