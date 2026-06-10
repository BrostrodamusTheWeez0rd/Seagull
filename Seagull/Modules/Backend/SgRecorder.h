#ifndef SGRECORDER_H
#define SGRECORDER_H

#include <QObject>
#include <QUrl>
#include <QString>

class QProcess;

// Records the currently-playing live stream to disk by running a parallel
// `ffmpeg -c copy` against the same resolved URLs VLC is playing. It's fully
// independent of playback (a second connection to the CDN), so starting/stopping
// it never disturbs the video. Controlled by the player's Record button.
class SgRecorder : public QObject {
    Q_OBJECT
public:
    explicit SgRecorder(QObject* parent = nullptr);
    ~SgRecorder() override;

    bool isRecording() const;
    QString outputFile() const; // the file being written right now ("" if not recording)

    // Start recording the stream. audioUrl may be empty (single combined stream);
    // when present it's muxed in as a second input. title seeds the file name,
    // referer is sent to hotlink-protected CDNs (as VLC does for playback).
    void start(const QUrl& videoUrl, const QUrl& audioUrl,
        const QString& referer, const QString& title);

    // Stop gracefully so the container is finalised (important for MP4).
    void stop();

    // VOD "clip": download just the [startMs, endMs] range of a non-live stream via
    // yt-dlp --download-sections (the page URL, re-resolved). One-shot, fire+forget.
    void clipSection(const QString& pageUrl, qint64 startMs, qint64 endMs, const QString& title);
    bool isClipping() const;
    void cancelClip(); // kill a running clip extraction (escape hatch)

signals:
    void started(const QString& filePath);
    void finished(const QString& filePath, bool ok);
    void clipFinished(const QString& filePath, bool ok);
    void logMessage(const QString& line);

private:
    QString ffmpegPath() const;     // tools/ffmpeg.exe next to the app
    QString ytDlpPath() const;      // tools/yt-dlp.exe
    QString toolsDir() const;       // the tools/ folder
    QString outputDir() const;      // config Paths/DownloadFolder
    QString extForFormat() const;   // config Streaming/RecordFormat -> mkv|mp4|ts
    QString mergeFormat() const;    // clip container (mp4|mkv — yt-dlp can't merge to ts)
    static QString sanitize(const QString& name); // strip path-illegal chars

    QProcess* m_proc = nullptr;
    QString   m_outFile;
    bool      m_stopping = false;

    QProcess* m_clipProc = nullptr; // separate process for VOD clip extraction
    QString   m_clipFile;
    bool      m_clipCancelled = false;
};

#endif // SGRECORDER_H
