#include "SgSearch.h"
#include "SgOptions.h" // cookieArgs() — browser cookies for age-gated / bot-checked sites
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>

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
    if (site == Site::PornHub) { // scraped over HTTP, not yt-dlp (richer cards)
        startPornHubSearch(query.trimmed(), limit);
        return;
    }
    if (site == Site::Chaturbate) { // live cam rooms via their JSON API
        startChaturbateSearch(query.trimmed(), limit);
        return;
    }

    const QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings"
        // Parse YouTube's relative "3 weeks ago" text into an approximate upload
        // timestamp, in the same flat response (no per-video requests).
        << "--extractor-args" << "youtubetab:approximate_date";
    args += SgOptions::cookieArgs(); // opted-in browser cookies (PH age gates, YT bot checks)

    // Per-site search target. New sites slot in here with their own branch.
    // (PornHub is handled above via the HTML-scrape path, not yt-dlp.)
    switch (site) {
    case Site::YouTube:
        args << QString("ytsearch%1:%2").arg(limit).arg(query.trimmed());
        break;
    case Site::PornHub:
        break; // unreachable; handled before the yt-dlp args are built
    }

    emit logMessage("Searching: " + query.trimmed());
    emit logMessage("yt-dlp " + args.join(' ')); // full command, visible in the Bdev console
    m_process->start(exePath, args);
}

void SgSearch::fetchChannelVideos(const QString& channelUrl, int limit) {
    if (channelUrl.trimmed().isEmpty()) { emit failed("No channel to open."); return; }

    cancel(); // a channel open supersedes any in-flight query

    // PornHub models/channels have no yt-dlp listing; scrape their /videos tab instead
    // (same tiles as search). YouTube falls through to the yt-dlp path below.
    if (channelUrl.contains("pornhub.com", Qt::CaseInsensitive)) {
        fetchPornHubModel(channelUrl.trimmed(), limit);
        return;
    }

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
         << "--extractor-args" << "youtubetab:approximate_date" // approx upload dates, same request
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
    if (m_channelReply) {
        m_channelReply->disconnect(this);
        m_channelReply->abort();
        m_channelReply->deleteLater();
        m_channelReply = nullptr;
    }
    if (m_phReply) {
        m_phReply->disconnect(this);
        m_phReply->abort();
        m_phReply->deleteLater();
        m_phReply = nullptr;
    }
    if (m_phModelReply) {
        m_phModelReply->disconnect(this);
        m_phModelReply->abort();
        m_phModelReply->deleteLater();
        m_phModelReply = nullptr;
    }
    if (m_cbReply) {
        m_cbReply->disconnect(this);
        m_cbReply->abort();
        m_cbReply->deleteLater();
        m_cbReply = nullptr;
    }
    m_buffer.clear();
}

void SgSearch::handleFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (m_cancelled) { m_cancelled = false; return; } // superseded by a newer query

    m_buffer.append(m_process->readAllStandardOutput());

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        const QString err = QString::fromLocal8Bit(m_buffer).trimmed();
        emit logMessage("Search failed (yt-dlp exit " + QString::number(exitCode) + "): "
            + (err.isEmpty() ? QStringLiteral("(no output)") : err.right(600)));
        emit failed(err.isEmpty() ? "Search failed." : ("Search failed: " + err.right(400)));
        return;
    }

    const int jsonStart = m_buffer.indexOf('{');
    if (jsonStart == -1) {
        emit logMessage("Search: no JSON in yt-dlp output. Raw: "
            + QString::fromLocal8Bit(m_buffer).trimmed().right(400));
        emit failed("No results.");
        return;
    }

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(m_buffer.mid(jsonStart), &perr);
    if (perr.error != QJsonParseError::NoError || doc.isNull()) {
        emit logMessage("Search: JSON parse error: " + perr.errorString());
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

    // parseYoutube is site-aware: it branches internally for non-YouTube layouts
    // (PornHub returns a plain flat list, no channel/shorts shelves).
    const QList<SearchResult> results = parseYoutube(doc.object());
    emit logMessage(QString("Search parsed %1 result(s).").arg(results.size()));
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
    // YouTube's thumbnails[] serve WebP that QPixmap can't decode, so build the
    // canonical /vi/<id>/mqdefault.jpg JPEG from the video id instead. Other sites
    // (PornHub) hand back real JPEGs in thumbnails[], so use the largest of those.
    const QString id = e["id"].toString();
    if (m_site == Site::YouTube && !id.isEmpty())
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
    // Approximate upload time, parsed by yt-dlp from YouTube's "3 weeks ago" text
    // (the youtubetab:approximate_date extractor-arg). Null when YouTube showed no
    // relative date for that result.
    r.timestamp = e.contains("timestamp") && !e["timestamp"].isNull()
        ? static_cast<qint64>(e["timestamp"].toDouble()) : -1;

    // Shorts rarely surface in normal yt-dlp search (the shorts shelf is skipped),
    // but tag any that do by their URL so the filter sees them.
    r.isShort = r.url.contains("/shorts/", Qt::CaseInsensitive);
    return true;
}

QList<SearchResult> SgSearch::parseYoutube(const QJsonObject& root) const {
    QList<SearchResult> out;
    const QJsonArray entries = root["entries"].toArray();

    // Non-YouTube sites (PornHub) return a plain flat list of video entries with no
    // channel/shorts shelves, so parse each straight through.
    if (m_site != Site::YouTube) {
        for (const auto& it : entries) {
            SearchResult r;
            if (parseVideoEntry(it.toObject(), r)) out.append(r);
        }
        return out;
    }

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

    // A flat-playlist channel listing omits per-entry uploader fields (every video
    // belongs to this one channel — the info lives on the root), so video cards from
    // it would lack the clickable channel name + star "profile" that normal search
    // cards show. Backfill the channel name/URL from the root onto each entry that
    // didn't carry its own.
    auto backfill = [&info](SearchResult& r) {
        if (r.channel.isEmpty())    r.channel    = info.channel;
        if (r.channelUrl.isEmpty()) r.channelUrl = info.channelUrl;
    };

    for (const auto& it : root["entries"].toArray()) {
        const QJsonObject e = it.toObject();
        // A channel root can come back as tabs/playlists nesting the videos one
        // level down — flatten that. (Targeting /videos usually avoids it.)
        if (e.contains("entries")) {
            for (const auto& it2 : e["entries"].toArray()) {
                SearchResult r;
                if (parseVideoEntry(it2.toObject(), r)) { backfill(r); videos.append(r); }
            }
            continue;
        }
        SearchResult r;
        if (parseVideoEntry(e, r)) { backfill(r); videos.append(r); }
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

// ---------------------------------------------------------------------------
// Channel search ("channel:" prefix) — YouTube internal search API
// ---------------------------------------------------------------------------
// yt-dlp can't reliably list channels by name, so (like shorts) go through
// youtubei/v1/search with the channel filter and parse channelRenderer. One page
// is plenty for a channel lookup; results are cached per query.

void SgSearch::searchChannels(const QString& query, int limit) {
    cancel(); // supersede any in-flight video/shorts/channel request

    if (!m_nam) m_nam = new QNetworkAccessManager(this);

    m_channelLimit = limit;
    if (query != m_channelQuery) {
        m_channelQuery = query;
        m_channelResults.clear();
        m_channelSeenIds.clear();
    }
    if (!m_channelResults.isEmpty()) { emit resultsReady(m_channelResults); return; } // cached

    emit logMessage("Searching channels: " + query);
    fetchChannelSearchPage();
}

void SgSearch::fetchChannelSearchPage() {
    QJsonObject client{ {"clientName", "WEB"}, {"clientVersion", "2.20250613.00.00"} };
    QJsonObject body{ {"context", QJsonObject{ {"client", client} }} };
    body["query"]  = m_channelQuery;
    body["params"] = "EgIQAg=="; // YouTube search filter: type = Channel

    QNetworkRequest req(QUrl("https://www.youtube.com/youtubei/v1/search?prettyPrint=false"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    m_channelReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_channelReply, &QNetworkReply::finished, this, &SgSearch::handleChannelSearchReply);
}

void SgSearch::handleChannelSearchReply() {
    QNetworkReply* reply = m_channelReply;
    m_channelReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed("Channel search failed: " + reply->errorString());
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (doc.isNull()) { emit failed("Could not parse channel results."); return; }

    QList<QJsonObject> renderers;
    collectObjects(doc.object(), "channelRenderer", renderers);

    for (const QJsonObject& c : renderers) {
        const QString channelId = c["channelId"].toString();
        // Prefer the handle URL (canonicalBaseUrl, e.g. "/@veritasium"); fall back
        // to the /channel/<id> form.
        QString url = c["navigationEndpoint"]["browseEndpoint"]["canonicalBaseUrl"].toString();
        if (url.startsWith("/")) url = "https://www.youtube.com" + url;
        if (url.isEmpty() && !channelId.isEmpty())
            url = "https://www.youtube.com/channel/" + channelId;
        if (url.isEmpty()) continue;

        const QString key = channelId.isEmpty() ? url : channelId;
        if (m_channelSeenIds.contains(key)) continue;

        QString name = c["title"]["simpleText"].toString();
        if (name.isEmpty()) name = c["title"]["runs"][0]["text"].toString();
        if (name.isEmpty()) continue;

        // Avatar: widest of thumbnail.thumbnails[] (urls are protocol-relative).
        QString avatar; int bestW = -1;
        for (const auto& t : c["thumbnail"]["thumbnails"].toArray()) {
            const QJsonObject to = t.toObject();
            const int w = to["width"].toInt();
            if (w > bestW) { bestW = w; avatar = to["url"].toString(); }
        }
        if (avatar.startsWith("//")) avatar = "https:" + avatar;

        // Pre-formatted text like "15.5M subscribers".
        QString subs = c["subscriberCountText"]["simpleText"].toString();
        if (subs.isEmpty())
            subs = c["subscriberCountText"]["accessibility"]["accessibilityData"]["label"].toString();

        SearchResult r;
        r.isChannel      = true;
        r.title          = name;
        r.channel        = name;
        r.url            = url;
        r.channelUrl     = url;
        r.thumbnail      = avatar;
        r.subscriberText = subs;
        m_channelSeenIds.insert(key);
        m_channelResults.append(r);
        if (m_channelResults.size() >= m_channelLimit) break;
    }

    emit resultsReady(m_channelResults);
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

// ---------------------------------------------------------------------------
// PornHub search — HTML scrape of the results page (one request per page)
// ---------------------------------------------------------------------------

namespace {
QString phUnescape(QString s) {
    s.replace("&amp;", "&");  s.replace("&lt;", "<");   s.replace("&gt;", ">");
    s.replace("&quot;", "\""); s.replace("&#039;", "'"); s.replace("&#39;", "'");
    s.replace("&apos;", "'");
    return s;
}
// "1.2M" / "12K" / "1,234" -> 1200000 / 12000 / 1234 (-1 if unparseable).
qint64 phParseCount(QString s) {
    s = s.trimmed();
    if (s.isEmpty()) return -1;
    double mult = 1.0;
    const QChar last = s.back().toUpper();
    if      (last == 'K') { mult = 1e3; s.chop(1); }
    else if (last == 'M') { mult = 1e6; s.chop(1); }
    else if (last == 'B') { mult = 1e9; s.chop(1); }
    s.remove(',').remove(' ');
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? static_cast<qint64>(v * mult) : -1;
}
// "12:34" / "1:02:03" -> seconds (-1 if unparseable).
qint64 phParseDuration(const QString& s) {
    const QStringList parts = s.trimmed().split(':');
    qint64 secs = 0;
    for (const QString& p : parts) {
        bool ok = false;
        const int n = p.trimmed().toInt(&ok);
        if (!ok) return -1;
        secs = secs * 60 + n;
    }
    return parts.isEmpty() ? -1 : secs;
}
// Derive a readable display name from a PornHub model URL's slug, e.g.
// ".../model/aria-skye/videos" -> "Aria Skye". Used as a fallback channel-header
// name when the page name can't be scraped.
QString phNameFromUrl(const QString& url) {
    QString u = url;
    while (u.endsWith('/')) u.chop(1);
    if (u.endsWith("/videos")) { u.chop(7); while (u.endsWith('/')) u.chop(1); }
    QString slug = u.section('/', -1);
    const int q = slug.indexOf('?'); if (q >= 0) slug = slug.left(q);
    slug.replace('-', ' ').replace('_', ' ');
    QStringList words = slug.split(' ', Qt::SkipEmptyParts);
    for (QString& w : words) w[0] = w[0].toUpper();
    return words.join(' ');
}
// Best-effort model display name from the page's first <h1>. Empty if not found
// or implausibly long (so the caller can fall back to the slug-derived name).
QString phModelName(const QString& html) {
    static const QRegularExpression h1Re(QStringLiteral("<h1[^>]*>(.*?)</h1>"),
                                         QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    const auto m = h1Re.match(html);
    if (!m.hasMatch()) return QString();
    QString name = m.captured(1);
    name.remove(tagRe);
    name = phUnescape(name).simplified();
    return name.size() > 60 ? QString() : name;
}
}

void SgSearch::startPornHubSearch(const QString& query, int limit) {
    if (!m_nam) m_nam = new QNetworkAccessManager(this);

    m_phLimit = limit;
    if (query != m_phQuery) { // new query resets the accumulated pages
        m_phQuery = query;
        m_phResults.clear();
        m_phSeen.clear();
        m_phPage = 0;
        m_phExhausted = false;
    }

    if (m_phResults.size() >= limit || m_phExhausted) { // enough cached / no more pages
        emit resultsReady(m_phResults);
        return;
    }

    emit logMessage("Searching PornHub: " + query);
    fetchPornHubPage();
}

void SgSearch::fetchPornHubPage() {
    const int page = m_phPage + 1;
    QUrl u("https://www.pornhub.com/video/search");
    QUrlQuery uq;
    uq.addQueryItem("search", m_phQuery);
    if (page > 1) uq.addQueryItem("page", QString::number(page));
    u.setQuery(uq);

    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    // Static age-gate bypass: a constant flag that means "18+", NOT the user's
    // browser cookies. Skips the interstitial the same way yt-dlp does.
    req.setRawHeader("Cookie", "age_verified=1; platform=pc; accessAgeDisclaimerPH=1; accessPH=1");
    req.setRawHeader("Accept-Language", "en-US,en;q=0.9");

    m_phReply = m_nam->get(req);
    connect(m_phReply, &QNetworkReply::finished, this, &SgSearch::handlePornHubReply);
}

void SgSearch::handlePornHubReply() {
    QNetworkReply* reply = m_phReply;
    m_phReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit logMessage("PornHub search failed: " + reply->errorString());
        if (!m_phResults.isEmpty()) emit resultsReady(m_phResults); // keep what we have
        else emit failed("PornHub search failed: " + reply->errorString());
        return;
    }

    ++m_phPage;
    const QList<SearchResult> page = parsePornHubHtml(QString::fromUtf8(reply->readAll()));
    emit logMessage(QString("PornHub page %1: %2 tile(s) parsed.").arg(m_phPage).arg(page.size()));

    const int before = m_phResults.size();
    for (const SearchResult& r : page) {
        if (r.url.isEmpty() || m_phSeen.contains(r.url)) continue;
        m_phSeen.insert(r.url);
        m_phResults.append(r);
    }
    if (m_phResults.size() == before || page.isEmpty()) m_phExhausted = true; // page added nothing new

    if (m_phResults.size() < m_phLimit && !m_phExhausted) {
        fetchPornHubPage(); // keep paging until we have enough for the requested batch
        return;
    }
    emit resultsReady(m_phResults);
}

QList<SearchResult> SgSearch::parsePornHubHtml(const QString& html) const {
    QList<SearchResult> out;

    // Each result tile carries data-video-vkey; slice the page into per-tile chunks
    // bounded by successive vkeys so a missing field in one tile can't bleed into the
    // next. Within a chunk we pull the first match of each field.
    static const QRegularExpression vkeyRe (QStringLiteral("data-video-vkey=\"([^\"]+)\""));
    static const QRegularExpression titAttr(QStringLiteral("class=\"title\"[^>]*>\\s*<a[^>]*?\\stitle=\"([^\"]+)\""));
    static const QRegularExpression titText(QStringLiteral("class=\"title\"[^>]*>\\s*<a[^>]*?>([^<]+)</a>"));
    static const QRegularExpression titData(QStringLiteral("data-title=\"([^\"]+)\""));
    static const QRegularExpression thumbRe(QStringLiteral("data-mediumthumb=\"([^\"]+)\""));
    static const QRegularExpression thumbAlt(QStringLiteral("data-image=\"([^\"]+)\""));
    static const QRegularExpression durRe  (QStringLiteral("<var class=\"duration\">([^<]+)</var>"));
    static const QRegularExpression viewsRe(QStringLiteral("class=\"views\">\\s*<var>([^<]+)</var>"));
    // Uploader/channel: the tile's usernameWrap block holds an <a> to the model/
    // channel/user page; we want its visible name (the anchor may also wrap a
    // verified badge, so strip any inner tags) and its href (the model page URL).
    // DotMatchesEverything spans newlines.
    static const QRegularExpression userRe(
        QStringLiteral("usernameWrap[^>]*>\\s*<a\\b[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression userHrefRe(
        QStringLiteral("usernameWrap[^>]*>\\s*<a\\b[^>]*href=\"([^\"]+)\""));
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));

    struct Hit { QString vkey; qsizetype pos; };
    QList<Hit> hits;
    auto vit = vkeyRe.globalMatch(html);
    while (vit.hasNext()) { const auto m = vit.next(); hits.append({ m.captured(1), m.capturedStart() }); }

    for (int i = 0; i < hits.size(); ++i) {
        const qsizetype start = hits[i].pos;
        const qsizetype end   = (i + 1 < hits.size()) ? hits[i + 1].pos : html.size();
        const QString chunk = html.mid(start, end - start);

        SearchResult r;
        r.url = "https://www.pornhub.com/view_video.php?viewkey=" + hits[i].vkey;

        // Title: prefer the title link's title attr, then its text, then data-title.
        QRegularExpressionMatch m = titAttr.match(chunk);
        if (!m.hasMatch()) m = titText.match(chunk);
        if (!m.hasMatch()) m = titData.match(chunk);
        r.title = phUnescape(m.captured(1)).trimmed();
        if (r.title.isEmpty()) continue; // not a usable video tile

        // Thumbnail: PornHub gives some tiles a real .jpg and others a dynamic CDN
        // URL (no extension) that Qt can't decode. Prefer a .jpg from either
        // attribute; fall back to whatever's present.
        QString thumbJpg, thumbAny;
        auto considerThumb = [&](const QRegularExpressionMatch& mm) {
            if (!mm.hasMatch()) return;
            const QString u = phUnescape(mm.captured(1));
            if (thumbAny.isEmpty()) thumbAny = u;
            if (thumbJpg.isEmpty() && (u.contains(".jpg", Qt::CaseInsensitive)
                                    || u.contains(".jpeg", Qt::CaseInsensitive)))
                thumbJpg = u;
        };
        considerThumb(thumbRe.match(chunk));
        considerThumb(thumbAlt.match(chunk));
        r.thumbnail = !thumbJpg.isEmpty() ? thumbJpg : thumbAny;
        // PornHub's transform CDN (CDN77) hotlink-protects thumbnails: without a
        // pornhub.com Referer it 403s. Harmless on the static thumbs that don't need it.
        if (!r.thumbnail.isEmpty()) r.thumbnailReferer = QStringLiteral("https://www.pornhub.com/");

        if (const auto md = durRe.match(chunk); md.hasMatch())
            r.duration = phParseDuration(md.captured(1));
        if (const auto mv = viewsRe.match(chunk); mv.hasMatch())
            r.viewCount = phParseCount(mv.captured(1));

        // Uploader/model name (strip inner badge tags, unescape, collapse space) and
        // the model page URL, so the card shows a clickable, favouritable uploader.
        if (const auto mu = userRe.match(chunk); mu.hasMatch()) {
            QString name = mu.captured(1);
            name.remove(tagRe);
            name = phUnescape(name).simplified();
            if (!name.isEmpty()) r.channel = name;
        }
        if (const auto mh = userHrefRe.match(chunk); mh.hasMatch()) {
            QString href = phUnescape(mh.captured(1)).trimmed();
            if (href.startsWith('/')) href = "https://www.pornhub.com" + href;
            if (href.startsWith("http")) r.channelUrl = href;
        }

        out.append(r);
    }
    return out;
}

void SgSearch::fetchPornHubModel(const QString& modelUrl, int /*limit*/) {
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    m_site = Site::PornHub;
    m_phModelUrl  = modelUrl;
    m_phModelName = phNameFromUrl(modelUrl); // slug fallback; page name preferred below

    // Target the model/channel's uploads tab, like the YouTube path appends /videos.
    QString url = modelUrl;
    while (url.endsWith('/')) url.chop(1);
    if (!url.endsWith("/videos")) url += "/videos";

    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    req.setRawHeader("Cookie", "age_verified=1; platform=pc; accessAgeDisclaimerPH=1; accessPH=1");
    req.setRawHeader("Accept-Language", "en-US,en;q=0.9");

    emit logMessage("Opening PornHub model: " + url);
    m_phModelReply = m_nam->get(req);
    connect(m_phModelReply, &QNetworkReply::finished, this, &SgSearch::handlePornHubModelReply);
}

void SgSearch::handlePornHubModelReply() {
    QNetworkReply* reply = m_phModelReply;
    m_phModelReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed("PornHub model page failed: " + reply->errorString());
        return;
    }

    const QString html = QString::fromUtf8(reply->readAll());
    const QList<SearchResult> videos = parsePornHubHtml(html); // same tiles as search

    SearchResult info;
    info.isChannel  = true;
    info.channelUrl = m_phModelUrl;
    const QString scraped = phModelName(html);
    info.channel = !scraped.isEmpty() ? scraped : m_phModelName;
    info.title   = info.channel;

    emit logMessage(QString("PornHub model: %1 video(s).").arg(videos.size()));
    emit channelVideosReady(info, videos);
}

// ---------------------------------------------------------------------------
// Chaturbate search — live cam rooms via the JSON room-list API
// ---------------------------------------------------------------------------

void SgSearch::startChaturbateSearch(const QString& query, int limit) {
    if (!m_nam) m_nam = new QNetworkAccessManager(this);

    m_cbLimit = limit;
    if (query != m_cbQuery) { // new query resets the accumulated rooms
        m_cbQuery = query;
        m_cbResults.clear();
        m_cbSeen.clear();
        m_cbExhausted = false;
    }
    if (m_cbResults.size() >= limit || m_cbExhausted) {
        emit resultsReady(m_cbResults);
        return;
    }
    emit logMessage("Searching Chaturbate: " + (query.isEmpty() ? QStringLiteral("(all rooms)") : query));
    fetchChaturbatePage();
}

void SgSearch::fetchChaturbatePage() {
    QUrl u("https://chaturbate.com/api/ts/roomlist/room-list/");
    QUrlQuery uq;
    uq.addQueryItem("limit", QString::number(qMax(20, m_cbLimit)));
    uq.addQueryItem("offset", QString::number(m_cbResults.size())); // offset paging
    uq.addQueryItem("genders", "f,m,t,c");                          // all room types
    if (!m_cbQuery.isEmpty()) uq.addQueryItem("tag", m_cbQuery);    // query = tag filter
    u.setQuery(uq);

    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    req.setRawHeader("X-Requested-With", "XMLHttpRequest"); // the API answers JSON to XHR
    m_cbReply = m_nam->get(req);
    connect(m_cbReply, &QNetworkReply::finished, this, &SgSearch::handleChaturbateReply);
}

void SgSearch::handleChaturbateReply() {
    QNetworkReply* reply = m_cbReply;
    m_cbReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit logMessage("Chaturbate search failed: " + reply->errorString());
        if (!m_cbResults.isEmpty()) emit resultsReady(m_cbResults);
        else emit failed("Chaturbate search failed: " + reply->errorString());
        return;
    }

    const QByteArray bytes = reply->readAll();
    const QList<SearchResult> page = parseChaturbateJson(bytes);
    const int totalCount = QJsonDocument::fromJson(bytes).object()["total_count"].toInt();
    emit logMessage(QString("Chaturbate: %1 room(s) parsed.").arg(page.size()));

    const int before = m_cbResults.size();
    for (const SearchResult& r : page) {
        if (r.url.isEmpty() || m_cbSeen.contains(r.url)) continue;
        m_cbSeen.insert(r.url);
        m_cbResults.append(r);
    }
    // No new rooms, or we've pulled the whole list -> stop paging.
    if (m_cbResults.size() == before || page.isEmpty()
        || (totalCount > 0 && m_cbResults.size() >= totalCount))
        m_cbExhausted = true;

    if (m_cbResults.size() < m_cbLimit && !m_cbExhausted) {
        fetchChaturbatePage();
        return;
    }
    emit resultsReady(m_cbResults);
}

QList<SearchResult> SgSearch::parseChaturbateJson(const QByteArray& bytes) const {
    QList<SearchResult> out;
    const QJsonArray rooms = QJsonDocument::fromJson(bytes).object()["rooms"].toArray();
    for (const auto& it : rooms) {
        const QJsonObject o = it.toObject();
        const QString user = o["username"].toString();
        if (user.isEmpty()) continue;

        SearchResult r;
        r.url = "https://chaturbate.com/" + user + "/"; // the player handles CB live rooms
        QString subj = o["room_subject"].toString();
        if (subj.isEmpty()) subj = o["subject"].toString();
        r.title = subj.isEmpty() ? user : subj;
        r.channel = user;
        r.channelUrl = r.url; // the model's room IS its "channel" — lets the card show a favourite star

        QString img = o["img"].toString();
        if (img.startsWith("//")) img = "https:" + img;
        r.thumbnail = img;
        if (!r.thumbnail.isEmpty()) r.thumbnailReferer = QStringLiteral("https://chaturbate.com/");

        if (o.contains("num_users") && !o["num_users"].isNull())
            r.viewCount = static_cast<qint64>(o["num_users"].toDouble()); // current viewers
        out.append(r);
    }
    return out;
}
