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

    // Site-aware stream resolution. YouTube serves separate adaptive video+audio,
    // so it gets a container-matched pair (audioUrl set -> caller uses input-slave).
    // Every other site (PornHub, generic, ...) gets a single already-muxed stream
    // (audioUrl empty). Caps to targetH (<=0 = no cap). Returns false if nothing
    // playable was found.
    bool resolveStream(const QJsonObject& root, int targetH,
        QString& videoUrl, QString& audioUrl);

    // "Auto" + one entry per distinct video height, for the quality menu.
    QList<StreamOption> buildQualityOptions(const QJsonObject& root);

}
