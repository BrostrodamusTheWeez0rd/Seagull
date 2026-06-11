#pragma once

#include <QObject>
#include <QPixmap>
#include <QStringList>

class QProcess;

// Local-file thumbnail generator for the Library's media cards. Videos get a
// frame grab, audio files get their embedded cover art — both via one ffmpeg
// process at a time (a strict FIFO, so opening a folder of 300 videos never
// fork-bombs). Images are just loaded scaled, no process. Results are cached
// on disk (cache/thumbs/, keyed by path+size+mtime) so cards are instant on
// every later visit.
class SgThumbnailer : public QObject {
    Q_OBJECT

public:
    explicit SgThumbnailer(QObject* parent = nullptr);

    // Queue a thumbnail for this file. Cache hits and plain images answer
    // immediately (still via the signal); misses run through the ffmpeg FIFO.
    // Files with no extractable image (e.g. coverless audio) emit nothing —
    // the card keeps its placeholder.
    void requestThumbnail(const QString& filePath);

    // Drop everything queued (e.g. the view is being rebuilt for another type).
    // The single in-flight process, if any, is left to finish.
    void cancelPending();

signals:
    void thumbnailReady(const QString& filePath, const QPixmap& pixmap);

private:
    void pump();                          // start ffmpeg for the queue head
    void onProcessFinished();
    QString cachePathFor(const QString& filePath) const;

    static bool isImageFile(const QString& path);
    static bool isAudioFile(const QString& path);

    QStringList m_queue;        // file paths waiting for ffmpeg
    QString     m_current;      // file being processed
    QString     m_currentOut;   // its cache/output path
    bool        m_retried = false; // video frame grab retried at t=0
    QProcess*   m_proc = nullptr;
};
