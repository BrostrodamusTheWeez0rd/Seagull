#pragma once
#include <QString>
#include <QList>

class QJsonArray;
class QJsonObject;

struct StreamOption {
    QString formatId;
    QString label;
    bool isAudioOnly;
};

// Pure stream-format selection over a yt-dlp `-J` JSON dump. No process, no I/O,
// no signals — just policy: given the format list, decide what to play/show.
namespace SgFormat {

    // Prefers the widest .jpg from thumbnails[] (QPixmap loads it without the webp
    // plugin); falls back to the single "thumbnail" field.
    QString pickThumbnail(const QJsonObject& root);

    // Height of the format with the given id, or -1 if not found ("no cap").
    int heightForFormatId(const QJsonArray& formats, const QString& id);

    // Container-matched video+audio pair (mp4+m4a, else webm+opus/vorbis) so VLC's
    // :input-slave merge stays in sync. Caps to targetH (<=0 = no cap). Returns
    // false if no split pair is available.
    bool chooseMatchedAvPair(const QJsonArray& formats, int targetH,
        QString& videoUrl, QString& audioUrl);

    // Best already-muxed (progressive) stream URL, or empty.
    QString bestProgressiveUrl(const QJsonArray& formats);

    // "Auto" + one entry per distinct video height, for the quality menu.
    QList<StreamOption> buildQualityOptions(const QJsonObject& root);

}
