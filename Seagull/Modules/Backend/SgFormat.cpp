#include "SgFormat.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

// Container-matched A/V selection (ported from the Python prototype's
// _choose_matched_av_pair). Pairing video and audio from the same container
// family is what keeps VLC's :input-slave merge from desyncing / dropping out.
namespace {
    bool isVideoOnly(const QJsonObject& f) {
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        return !vc.isEmpty() && vc != "none" && (ac.isEmpty() || ac == "none");
    }
    bool isAudioOnly(const QJsonObject& f) {
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        return !ac.isEmpty() && ac != "none" && (vc.isEmpty() || vc == "none");
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

bool SgFormat::chooseMatchedAvPair(const QJsonArray& formats, int targetH,
    QString& videoUrl, QString& audioUrl) {

    auto pickFamily = [&](const QStringList& vExts, const QStringList& aExts,
        const QStringList& aCodecPrefixes, QString& vUrl, QString& aUrl) -> bool {

        // Best video in this family at or below the target height (no AV1).
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

        // Best audio in this family, preferring the same extension as the video.
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

QString SgFormat::bestProgressiveUrl(const QJsonArray& formats) {
    QString url; int best = -1;
    for (const auto& it : formats) {
        QJsonObject f = it.toObject();
        QString vc = f["vcodec"].toString();
        QString ac = f["acodec"].toString();
        if (vc.isEmpty() || vc == "none" || ac.isEmpty() || ac == "none") continue;
        if (f["url"].toString().isEmpty()) continue;
        int h = f["height"].toInt();
        if (h > best) { best = h; url = f["url"].toString(); }
    }
    return url;
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
