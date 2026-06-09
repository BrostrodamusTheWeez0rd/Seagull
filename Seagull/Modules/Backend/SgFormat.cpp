#include "SgFormat.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

// Format-selection helpers. Container-matched A/V pairing (ported from the Python
// prototype's _choose_matched_av_pair) keeps VLC's :input-slave merge in sync for
// YouTube; the rest hands VLC a single muxed stream for sites that serve one.
namespace {
    bool isVideoOnly(const QJsonObject& f) {
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        return !vc.isEmpty() && vc != "none" && (ac.isEmpty() || ac == "none");
    }
    bool isAudioOnly(const QJsonObject& f) {
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        const bool noVideo = (vc.isEmpty() || vc == "none");
        const bool hasAudioCodec = !ac.isEmpty() && ac != "none";
        // Some extractors (e.g. Chaturbate) leave acodec blank on their audio-only
        // HLS track, so also treat a no-video format as audio when it advertises an
        // audio bitrate or names itself "audio".
        const bool looksAudio = hasAudioCodec
            || f["abr"].toDouble() > 0
            || f["format_id"].toString().contains("audio", Qt::CaseInsensitive);
        return noVideo && looksAudio;
    }
    bool isMuxed(const QJsonObject& f) {
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        return !vc.isEmpty() && vc != "none" && !ac.isEmpty() && ac != "none";
    }
    // Higher is better: height, then fps, then total bitrate.
    bool betterVideo(const QJsonObject& a, const QJsonObject& b) {
        int ah = a["height"].toInt(), bh = b["height"].toInt();
        if (ah != bh) return ah > bh;
        int af = a["fps"].toInt(), bf = b["fps"].toInt();
        if (af != bf) return af > bf;
        return a["tbr"].toDouble() > b["tbr"].toDouble();
    }
    // Higher is better: audio bitrate, then total bitrate.
    bool betterAudio(const QJsonObject& a, const QJsonObject& b) {
        double aa = a["abr"].toDouble(), ba = b["abr"].toDouble();
        if (aa != ba) return aa > ba;
        return a["tbr"].toDouble() > b["tbr"].toDouble();
    }
    // Playable URL for a format: the direct url, else the HLS/DASH manifest url
    // (m3u8/mpd) which VLC can open just as well.
    QString fmtUrl(const QJsonObject& f) {
        QString u = f["url"].toString();
        return u.isEmpty() ? f["manifest_url"].toString() : u;
    }

    // YouTube-style: best container-matched separate video + audio (no AV1), capped.
    bool chooseMatchedAvPair(const QJsonArray& formats, int targetH,
        QString& videoUrl, QString& audioUrl) {

        auto pickFamily = [&](const QStringList& vExts, const QStringList& aExts,
            const QStringList& aCodecPrefixes, QString& vUrl, QString& aUrl) -> bool {

            QJsonObject bestV; bool haveV = false;
            for (const auto& it : formats) {
                QJsonObject f = it.toObject();
                if (!isVideoOnly(f)) continue;
                if (!vExts.contains(f["ext"].toString().toLower())) continue;
                if (f["url"].toString().isEmpty()) continue;
                int h = f["height"].toInt();
                if (h <= 0) continue;
                if (targetH > 0 && h > targetH) continue;
                if (f["vcodec"].toString().toLower().startsWith("av01")) continue;
                if (!haveV || betterVideo(f, bestV)) { bestV = f; haveV = true; }
            }
            if (!haveV) return false;

            QString vext = bestV["ext"].toString().toLower();
            QString wantAext = (vext == "mp4" || vext == "m4v") ? "m4a" : vext;
            QJsonObject bestA;       bool haveA = false;
            QJsonObject bestASame;   bool haveASame = false;
            for (const auto& it : formats) {
                QJsonObject f = it.toObject();
                if (!isAudioOnly(f)) continue;
                QString ext = f["ext"].toString().toLower();
                if (!aExts.contains(ext)) continue;
                if (f["url"].toString().isEmpty()) continue;
                QString ac = f["acodec"].toString().toLower();
                bool okCodec = false;
                for (const auto& p : aCodecPrefixes) if (ac.startsWith(p)) { okCodec = true; break; }
                if (!okCodec) continue;
                if (!haveA || betterAudio(f, bestA)) { bestA = f; haveA = true; }
                if (ext == wantAext && (!haveASame || betterAudio(f, bestASame))) { bestASame = f; haveASame = true; }
            }
            if (!haveA) return false;

            vUrl = bestV["url"].toString();
            aUrl = (haveASame ? bestASame : bestA)["url"].toString();
            return true;
        };

        if (pickFamily({ "mp4","m4v" }, { "m4a","mp4" }, { "mp4a","aac" }, videoUrl, audioUrl)) return true;
        if (pickFamily({ "webm" }, { "webm" }, { "opus","vorbis" }, videoUrl, audioUrl)) return true;
        return false;
    }

    // Best already-muxed (progressive/HLS) stream URL at or below targetH.
    QString bestMuxedUrl(const QJsonArray& formats, int targetH) {
        QString url; int best = -1;
        for (const auto& it : formats) {
            QJsonObject f = it.toObject();
            if (!isMuxed(f)) continue;
            if (fmtUrl(f).isEmpty()) continue;
            int h = f["height"].toInt();
            if (targetH > 0 && h > targetH) continue;
            if (h > best) { best = h; url = fmtUrl(f); }
        }
        return url;
    }

    // Generic separate-stream pairing (any container, no AV1), for non-YouTube
    // sites that still split video and audio.
    bool chooseGenericAvPair(const QJsonArray& formats, int targetH,
        QString& videoUrl, QString& audioUrl) {
        QJsonObject bestV; bool haveV = false;
        for (const auto& it : formats) {
            QJsonObject f = it.toObject();
            if (!isVideoOnly(f)) continue;
            if (fmtUrl(f).isEmpty()) continue;
            int h = f["height"].toInt();
            if (h <= 0) continue;
            if (targetH > 0 && h > targetH) continue;
            if (f["vcodec"].toString().toLower().startsWith("av01")) continue;
            if (!haveV || betterVideo(f, bestV)) { bestV = f; haveV = true; }
        }
        if (!haveV) return false;

        QJsonObject bestA; bool haveA = false;
        for (const auto& it : formats) {
            QJsonObject f = it.toObject();
            if (!isAudioOnly(f)) continue;
            if (fmtUrl(f).isEmpty()) continue;
            if (!haveA || betterAudio(f, bestA)) { bestA = f; haveA = true; }
        }
        if (!haveA) return false;

        videoUrl = fmtUrl(bestV);
        audioUrl = fmtUrl(bestA);
        return true;
    }

    // Last resort: the highest format that carries any playable URL.
    QString anyPlayableUrl(const QJsonArray& formats) {
        QString url; int best = -1;
        for (const auto& it : formats) {
            QJsonObject f = it.toObject();
            if (fmtUrl(f).isEmpty()) continue;
            int h = f["height"].toInt();
            if (h > best) { best = h; url = fmtUrl(f); }
        }
        return url;
    }
}

QString SgFormat::pickThumbnail(const QJsonObject& root) {
    QString thumb;
    const QJsonArray thumbs = root["thumbnails"].toArray();
    int bestW = -1;
    for (const auto& it : thumbs) {
        QJsonObject t = it.toObject();
        QString u = t["url"].toString();
        if (u.isEmpty()) continue;
        if (!u.contains(".jpg", Qt::CaseInsensitive)) continue;
        int w = t["width"].toInt();
        if (w > bestW) { bestW = w; thumb = u; }
    }
    if (thumb.isEmpty()) thumb = root["thumbnail"].toString();
    return thumb;
}

int SgFormat::heightForFormatId(const QJsonArray& formats, const QString& id) {
    for (const auto& it : formats) {
        QJsonObject f = it.toObject();
        if (f["format_id"].toString() == id) return f["height"].toInt();
    }
    return -1; // not found -> treat as "no cap"
}

bool SgFormat::resolveStream(const QJsonObject& root, int targetH,
    QString& videoUrl, QString& audioUrl) {

    const QJsonArray formats = root["formats"].toArray();
    videoUrl.clear();
    audioUrl.clear();

    const bool isYoutube = root["extractor_key"].toString().toLower().startsWith("youtube")
        || root["extractor"].toString().toLower().startsWith("youtube");

    const bool isLive = root["is_live"].toBool();

    // YouTube: container-matched separate video+audio so VLC's input-slave merge
    // stays in sync. Unchanged behaviour. (audioUrl set -> caller pairs them.)
    //
    // Live is the exception: the matched pair forces the :demux=avformat +
    // :input-slave path, which treats a live DASH input as finite and hits EOF
    // after the first window (~a minute), ending playback. For live we fall
    // through to the single muxed/HLS manifest below so VLC's native demuxer
    // keeps refreshing the live playlist instead.
    if (isYoutube && !isLive && chooseMatchedAvPair(formats, targetH, videoUrl, audioUrl))
        return true;

    // Everything else (and the rare YouTube fallback): a single muxed stream that
    // plays directly — most sites serve progressive/HLS.
    audioUrl.clear();

    videoUrl = bestMuxedUrl(formats, targetH);
    if (!videoUrl.isEmpty()) return true;   // YouTube/Twitch live: muxed variant carries audio

    // yt-dlp's own resolved URL (set when it picked a single format).
    videoUrl = root["url"].toString();
    if (!videoUrl.isEmpty()) return true;

    // A non-YouTube site that still splits streams in some other container. Covers
    // sites like Chaturbate that serve separate video-only + audio-only HLS — the
    // caller builds a local master playlist from the returned pair (the site's own
    // master manifest is single-use, so VLC can't reuse it). See PlaybackEngine.
    if (chooseGenericAvPair(formats, targetH, videoUrl, audioUrl))
        return true;

    // Last resort: anything with a playable URL.
    audioUrl.clear();
    videoUrl = anyPlayableUrl(formats);
    return !videoUrl.isEmpty();
}

QList<StreamOption> SgFormat::buildQualityOptions(const QJsonObject& root) {
    QJsonArray formats = root["formats"].toArray();
    QList<StreamOption> options;
    QList<int> seenHeights;

    options.append({ "", "Auto", false });

    for (int i = formats.size() - 1; i >= 0; --i) {
        QJsonObject fmt = formats[i].toObject();
        int height = fmt["height"].toInt();
        QString vcodec = fmt["vcodec"].toString();

        if (height > 0 && vcodec != "none" && !seenHeights.contains(height)) {
            seenHeights.append(height);
            options.append({
                fmt["format_id"].toString(),
                QString::number(height) + "p",
                false
                });
        }
    }
    return options;
}
