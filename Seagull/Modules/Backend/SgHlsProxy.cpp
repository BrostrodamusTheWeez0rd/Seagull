#include "SgHlsProxy.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QDateTime>
#include <QDebug>
#include <algorithm>

namespace {
const QByteArray kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
const QByteArray kPlaylistMime = "application/vnd.apple.mpegurl";

// Twitch's public (anonymous) web client id and the PlaybackAccessToken persisted query.
const QByteArray kTwitchClientId = "kimne78kx3ncx6brgo4mv6wki5h1ko";
const QString kPlaybackAccessTokenHash = "ed230aa1e33e07eebb8928504583da78a5173989fadfb1ac94be06a04f3cdbe9";
// Player types that get served a clean (un-stitched) stream, tried in order.
const QStringList kPlayerTypes = { "embed", "popout", "autoplay" };

// Keep the per-stream URL dedup map from growing without bound on a long stream.
const int kMaxTrackedSegments = 256;

// Clients polling the same playlist within this window share one upstream fetch
// (player + recorder = two pollers; doubled usher polling makes Twitch freeze
// the playlist window, which starves every consumer). Comfortably under the
// 2s segment duration, so the live edge never lags more than one poll.
const qint64 kManifestReuseMs = 1200;

// True if this #EXTINF segment is one of Twitch's stitched ads.
bool isAdTitle(const QString& title) {
    return title.contains("Amazon", Qt::CaseInsensitive)
        || title.contains("stitched", Qt::CaseInsensitive);
}

QString randomHex(int bytes) {
    QString s;
    for (int i = 0; i < bytes; ++i)
        s += QString("%1").arg(QRandomGenerator::global()->bounded(256), 2, 16, QChar('0'));
    return s;
}
}

SgHlsProxy::SgHlsProxy(QObject* parent) : QObject(parent) {
    m_server = new QTcpServer(this);
    m_nam = new QNetworkAccessManager(this);
    m_token = QString::number(QRandomGenerator::global()->generate64(), 16);
    m_deviceId = randomHex(16);

    connect(m_server, &QTcpServer::newConnection, this, &SgHlsProxy::onConnection);
    const bool ok = m_server->listen(QHostAddress::LocalHost, 0); // ephemeral port
    qDebug() << "[HlsProxy] listen=" << ok << "port=" << (ok ? m_server->serverPort() : 0);
}

bool SgHlsProxy::isListening() const {
    return m_server && m_server->isListening();
}

QString SgHlsProxy::localBase() const {
    return "http://127.0.0.1:" + QString::number(m_server->serverPort());
}

QString SgHlsProxy::proxyUrlFor(const QString& absoluteUrl, const QString& referer) const {
    // Path ends in .m3u8 so VLC/ffmpeg detect HLS by extension as well as MIME type.
    return localBase() + "/hls.m3u8?t=" + m_token
        + "&r=" + QString::fromUtf8(QUrl::toPercentEncoding(referer))
        + "&u=" + QString::fromUtf8(QUrl::toPercentEncoding(absoluteUrl));
}

QUrl SgHlsProxy::proxify(const QUrl& upstream, const QString& referer) {
    if (!isListening() || !upstream.isValid() || upstream.isEmpty()) return upstream;
    return QUrl(proxyUrlFor(upstream.toString(), referer));
}

QUrl SgHlsProxy::proxifyTwitch(const QString& login, int height, const QUrl& fallbackVariant, const QString& referer) {
    if (!isListening() || login.isEmpty()) return fallbackVariant;
    const QString url = localBase() + "/twitch.m3u8?t=" + m_token
        + "&login=" + QString::fromUtf8(QUrl::toPercentEncoding(login))
        + "&h=" + QString::number(height)
        + "&r=" + QString::fromUtf8(QUrl::toPercentEncoding(referer))
        + "&u=" + QString::fromUtf8(QUrl::toPercentEncoding(fallbackVariant.toString()));
    qDebug() << "[HlsProxy] proxifyTwitch login=" << login << "height=" << height;
    return QUrl(url);
}

void SgHlsProxy::onConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* sock = m_server->nextPendingConnection();
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            const QByteArray data = sock->peek(16384);
            const int eol = data.indexOf("\r\n");
            if (eol < 0) return; // request line still arriving
            disconnect(sock, &QTcpSocket::readyRead, this, nullptr); // handle once
            const QStringList parts = QString::fromUtf8(data.left(eol)).split(' ');
            if (parts.size() < 2 || parts[0] != "GET") {
                writeResponse(sock, 400, "text/plain", "bad request");
                sock->disconnectFromHost();
                return;
            }
            handleRequest(sock, parts[1]);
            });
    }
}

void SgHlsProxy::handleRequest(QTcpSocket* sock, const QString& target) {
    const QUrl req("http://localhost" + target);
    const QUrlQuery q(req);
    if (q.queryItemValue("t") != m_token) {
        qWarning() << "[HlsProxy] 403 token mismatch";
        writeResponse(sock, 403, "text/plain", "forbidden");
        sock->disconnectFromHost();
        return;
    }
    const QString referer = q.queryItemValue("r", QUrl::FullyDecoded);

    if (req.path() == "/twitch.m3u8") {
        const QString login = q.queryItemValue("login", QUrl::FullyDecoded);
        const int height = q.queryItemValue("h").toInt();
        const QUrl fallback(q.queryItemValue("u", QUrl::FullyDecoded));
        serveTwitch(sock, login, height, referer, fallback);
        return;
    }

    const QUrl upstream(q.queryItemValue("u", QUrl::FullyDecoded));
    if (!upstream.isValid() || upstream.isEmpty()) {
        qWarning() << "[HlsProxy] 400 bad upstream";
        writeResponse(sock, 400, "text/plain", "missing upstream");
        sock->disconnectFromHost();
        return;
    }
    serveProxied(sock, upstream, referer);
}

// ---------------------------------------------------------------------------
// Twitch embed-token resolution (primary path, ad-free)
// ---------------------------------------------------------------------------

void SgHlsProxy::serveTwitch(QTcpSocket* sock, const QString& login, int height,
                             const QString& referer, const QUrl& fallback) {
    const QString key = login + "|" + QString::number(height);
    const QString cached = m_twitchVariant.value(key);
    if (!cached.isEmpty()) {
        // Already resolved a clean variant for this stream — just poll it.
        serveProxied(sock, QUrl(cached), referer);
        return;
    }
    resolveTwitch(sock, login, height, referer, fallback, 0);
}

void SgHlsProxy::resolveTwitch(QTcpSocket* sock, const QString& login, int height,
                               const QString& referer, const QUrl& fallback, int idx) {
    QPointer<QTcpSocket> safeSock = sock;
    if (idx >= kPlayerTypes.size()) {
        // No clean token worked — fall back to filtering yt-dlp's (ad-stitched) URL.
        qWarning() << "[HlsProxy] Twitch: no clean token; falling back to ad-strip of yt-dlp URL";
        if (safeSock) serveProxied(safeSock, fallback, referer);
        return;
    }
    const QString playerType = kPlayerTypes[idx];

    QJsonObject vars{
        { "isLive", true }, { "login", login }, { "isVod", false }, { "vodID", "" },
        { "playerType", playerType }, { "platform", playerType == "autoplay" ? "android" : "web" }
    };
    QJsonObject pq{ { "version", 1 }, { "sha256Hash", kPlaybackAccessTokenHash } };
    QJsonObject body{
        { "operationName", "PlaybackAccessToken" },
        { "variables", vars },
        { "extensions", QJsonObject{ { "persistedQuery", pq } } }
    };

    QNetworkRequest greq(QUrl("https://gql.twitch.tv/gql"));
    greq.setRawHeader("Client-ID", kTwitchClientId);
    greq.setRawHeader("X-Device-Id", m_deviceId.toUtf8());
    greq.setRawHeader("Content-Type", "text/plain;charset=UTF-8");

    QNetworkReply* reply = m_nam->post(greq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, safeSock, login, height, referer, fallback, idx, playerType]() {
            reply->deleteLater();
            if (!safeSock) return;

            const QJsonObject tok = QJsonDocument::fromJson(reply->readAll()).object()
                ["data"].toObject()["streamPlaybackAccessToken"].toObject();
            const QString sig = tok["signature"].toString();
            const QString value = tok["value"].toString();
            if (reply->error() != QNetworkReply::NoError || sig.isEmpty() || value.isEmpty()) {
                qWarning() << "[HlsProxy] Twitch token (" << playerType << ") failed:" << reply->errorString();
                resolveTwitch(safeSock, login, height, referer, fallback, idx + 1);
                return;
            }

            const QString usher = "https://usher.ttvnw.net/api/channel/hls/" + login + ".m3u8"
                "?allow_source=true&fast_bread=true&player_backend=mediaplayer"
                "&playlist_include_framerate=true&supported_codecs=h264"
                "&sig=" + QString::fromUtf8(QUrl::toPercentEncoding(sig))
                + "&token=" + QString::fromUtf8(QUrl::toPercentEncoding(value));

            QNetworkRequest ureq{ QUrl(usher) };
            ureq.setRawHeader("User-Agent", kUserAgent);
            ureq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply* ureply = m_nam->get(ureq);
            connect(ureply, &QNetworkReply::finished, this,
                [this, ureply, safeSock, login, height, referer, fallback, idx, playerType]() {
                    ureply->deleteLater();
                    if (!safeSock) return;
                    const QString master = QString::fromUtf8(ureply->readAll());
                    const QString variant = pickVariant(master, height);
                    if (ureply->error() != QNetworkReply::NoError || variant.isEmpty()) {
                        qWarning() << "[HlsProxy] Twitch usher (" << playerType << ") failed:" << ureply->errorString();
                        resolveTwitch(safeSock, login, height, referer, fallback, idx + 1);
                        return;
                    }
                    m_twitchVariant.insert(login + "|" + QString::number(height), variant);
                    qDebug() << "[HlsProxy] Twitch: clean stream via playerType=" << playerType
                             << "variant=" << variant.left(70);
                    serveProxied(safeSock, QUrl(variant), referer);
                });
        });
}

QString SgHlsProxy::pickVariant(const QString& masterText, int height) const {
    // Collect (height, url) for each #EXT-X-STREAM-INF video variant.
    QList<QPair<int, QString>> variants;
    const QStringList lines = masterText.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (!lines[i].trimmed().startsWith("#EXT-X-STREAM-INF")) continue;
        const QString inf = lines[i];
        int h = 0;
        const int ri = inf.indexOf("RESOLUTION=");
        if (ri >= 0) {
            const QString res = inf.mid(ri + 11);       // e.g. 1920x1080,...
            const int x = res.indexOf('x');
            if (x >= 0) {
                int j = x + 1; QString num;
                while (j < res.size() && res[j].isDigit()) num += res[j++];
                h = num.toInt();
            }
        }
        QString url;
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString u = lines[j].trimmed();
            if (u.isEmpty() || u.startsWith('#')) continue;
            url = u; break;
        }
        if (h > 0 && !url.isEmpty()) variants.append({ h, url });
    }
    if (variants.isEmpty()) return QString();

    std::sort(variants.begin(), variants.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    if (height <= 0) return variants.last().second; // best
    QString pick;
    for (const auto& v : variants) if (v.first <= height) pick = v.second; // largest <= height
    return pick.isEmpty() ? variants.first().second : pick; // else smallest
}

// ---------------------------------------------------------------------------
// Serve + ad-strip a media playlist (shared by both paths)
// ---------------------------------------------------------------------------

void SgHlsProxy::serveProxied(QTcpSocket* sock, const QUrl& upstream, const QString& referer) {
    // Fan-out: the player and the recorder poll this same playlist. Everyone
    // inside the reuse window gets the response from one upstream fetch, so a
    // second consumer never increases how often Twitch sees us poll (doubled
    // polling freezes the playlist window server-side, starving every client).
    {
        StreamState& st = m_streams[upstream.toString()];
        if (!st.lastBody.isEmpty()
            && QDateTime::currentMSecsSinceEpoch() - st.lastBodyAtMs < kManifestReuseMs) {
            writeResponse(sock, 200, kPlaylistMime, st.lastBody);
            sock->disconnectFromHost();
            return;
        }
    }

    QNetworkRequest req(upstream);
    req.setRawHeader("User-Agent", kUserAgent);
    if (!referer.isEmpty()) req.setRawHeader("Referer", referer.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QPointer<QTcpSocket> safeSock = sock;
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, safeSock, upstream, referer]() {
        reply->deleteLater();
        if (!safeSock) return;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[HlsProxy] upstream fetch FAILED:" << reply->errorString();
            // If a resolved Twitch variant went stale (embed token expires ~1h), drop it
            // from the cache so the next poll re-resolves a fresh one instead of looping.
            const QString up = upstream.toString();
            for (auto it = m_twitchVariant.begin(); it != m_twitchVariant.end(); ) {
                if (it.value() == up) it = m_twitchVariant.erase(it);
                else ++it;
            }
            // Bridge a transient upstream error with the last playlist we built —
            // a single bad poll must not kill the player (and with it a recording).
            const StreamState& st = m_streams[up];
            if (!st.lastBody.isEmpty())
                writeResponse(safeSock, 200, kPlaylistMime, st.lastBody);
            else
                writeResponse(safeSock, 502, "text/plain", "upstream error");
            safeSock->disconnectFromHost();
            return;
        }

        const QString text = QString::fromUtf8(reply->readAll());
        if (isMasterPlaylist(text)) {
            writeResponse(safeSock, 200, kPlaylistMime, filterMaster(text, upstream, referer));
            safeSock->disconnectFromHost();
            return;
        }

        StreamState& st = m_streams[upstream.toString()];
        const QByteArray filtered = filterMedia(text, upstream, st);
        st.lastBody = filtered;
        st.lastBodyAtMs = QDateTime::currentMSecsSinceEpoch();
        writeResponse(safeSock, 200, kPlaylistMime, filtered);
        safeSock->disconnectFromHost();
        });
}

bool SgHlsProxy::isMasterPlaylist(const QString& text) const {
    return text.contains("#EXT-X-STREAM-INF");
}

QByteArray SgHlsProxy::filterMaster(const QString& text, const QUrl& base, const QString& referer) const {
    auto resolve = [&](const QString& uri) -> QString {
        const QUrl u(uri.trimmed());
        return u.isRelative() ? base.resolved(u).toString() : uri.trimmed();
    };

    QStringList out;
    const QStringList lines = text.split('\n');
    for (const QString& raw : lines) {
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;
        if (t.startsWith('#'))
            out << raw;                              // header / #EXT-X-STREAM-INF etc.
        else
            out << proxyUrlFor(resolve(t), referer); // route each variant through us too
    }
    return (out.join('\n') + "\n").toUtf8();
}

QByteArray SgHlsProxy::filterMedia(const QString& text, const QUrl& base, StreamState& st) {
    auto resolve = [&](const QString& uri) -> QString {
        const QUrl u(uri.trimmed());
        return u.isRelative() ? base.resolved(u).toString() : uri.trimmed();
    };
    auto assignSeq = [&](const QString& absUrl) -> quint64 {
        if (!st.seqOf.contains(absUrl)) {
            st.seqOf.insert(absUrl, st.nextSeq++);
            st.order << absUrl;
        }
        return st.seqOf.value(absUrl);
    };

    QString pendingExtinf;
    bool nextIsAd = false;

    QList<Seg> real;                       // non-ad segments this refresh
    QList<QPair<QString, QString>> ads;    // (extinf, absUrl) for ad segments this refresh

    const QStringList lines = text.split('\n');
    for (const QString& raw : lines) {
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;

        if (t.startsWith('#')) {
            if (t.startsWith("#EXT-X-VERSION:"))
                st.version = t.mid(QString("#EXT-X-VERSION:").size()).toInt();
            else if (t.startsWith("#EXT-X-TARGETDURATION:"))
                st.targetDuration = t.mid(QString("#EXT-X-TARGETDURATION:").size()).toInt();

            if ((t.startsWith("#EXT-X-DATERANGE") && t.contains("twitch-stitched-ad"))
                || t.startsWith("#EXT-X-SCTE35") || t.startsWith("#EXT-X-CUE")
                || t.startsWith("#EXT-X-TWITCH-PREFETCH")
                || t.startsWith("#EXT-X-DISCONTINUITY"))
                continue;

            if (t.startsWith("#EXTINF")) {
                const int comma = t.indexOf(',');
                const QString title = comma >= 0 ? t.mid(comma + 1).trimmed() : QString();
                nextIsAd = isAdTitle(title);
                pendingExtinf = raw;
            }
        }
        else {
            const QString absUrl = resolve(t);
            if (nextIsAd) ads.append({ pendingExtinf, absUrl });
            else          real.append({ assignSeq(absUrl), pendingExtinf, absUrl });
            pendingExtinf.clear();
            nextIsAd = false;
        }
    }

    while (st.order.size() > kMaxTrackedSegments)
        st.seqOf.remove(st.order.takeFirst());

    if (!real.isEmpty()) {
        if (st.servingPreroll) {
            st.servingPreroll = false;
            st.discontinuitySeq = real.first().seq;
        }
        st.lastWindow = real;
        return buildMediaPlaylist(st, real);
    }

    if (!st.lastWindow.isEmpty())
        return buildMediaPlaylist(st, st.lastWindow); // mid-roll: hold last real window

    // Preroll with no history: serve the ad segments themselves (they decode cleanly);
    // we cut to live above once Twitch lists real segments. With the embed token this
    // branch should rarely fire.
    st.servingPreroll = true;
    QList<Seg> adSegs;
    for (const auto& a : ads)
        adSegs.append({ assignSeq(a.second), a.first, a.second });
    return buildMediaPlaylist(st, adSegs);
}

QByteArray SgHlsProxy::buildMediaPlaylist(const StreamState& st, const QList<Seg>& window) const {
    QStringList out;
    out << "#EXTM3U";
    out << "#EXT-X-VERSION:" + QString::number(st.version);
    out << "#EXT-X-TARGETDURATION:" + QString::number(st.targetDuration);
    out << "#EXT-X-MEDIA-SEQUENCE:" + QString::number(window.isEmpty() ? st.nextSeq : window.first().seq);
    for (const Seg& s : window) {
        if (s.seq == st.discontinuitySeq)
            out << "#EXT-X-DISCONTINUITY";
        out << (s.extinf.isEmpty() ? QString("#EXTINF:%1,").arg(st.targetDuration) : s.extinf);
        out << s.url;
    }
    return (out.join('\n') + "\n").toUtf8();
}

void SgHlsProxy::writeResponse(QTcpSocket* sock, int code, const QByteArray& contentType, const QByteArray& body) {
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;
    QByteArray resp = "HTTP/1.1 " + QByteArray::number(code) + " OK\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Cache-Control: no-cache\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    sock->write(resp);
    sock->flush();
}
