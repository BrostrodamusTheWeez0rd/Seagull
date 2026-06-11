#ifndef SGRECORDER_H
#define SGRECORDER_H

#include <QObject>
#include <QUrl>
#include <QString>
#include <QElapsedTimer>

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

    // Start recording via parallel ffmpeg -c copy. Used for LIVE streams (capture in
    // real time) and for LOCAL files (a lossless remux to the record format). audioUrl
    // may be empty (single combined stream); when present it's muxed as a second input.
    // A local-file videoUrl is fed to ffmpeg by native path. title seeds the file name;
    // referer is sent to hotlink-protected CDNs (as VLC does for playback).
    void start(const QUrl& videoUrl, const QUrl& audioUrl,
        const QString& referer, const QString& title);

    // Stop gracefully so the container is finalised (important for MP4).
    void stop();

    // VOD "clip": save just the [startMs, endMs] range of a non-live stream.
    // videoUrl/audioUrl are the CDN URLs the player already resolved (the streams
    // VLC is playing) — the section is cut straight from them with ffmpeg, no
    // yt-dlp relaunch/re-resolve. pageUrl is the Referer + the fallback (full
    // yt-dlp download + trim) when the direct cut is throttled, stale, or fails.
    void clipSection(const QString& pageUrl, const QUrl& videoUrl, const QUrl& audioUrl,
        qint64 startMs, qint64 endMs, const QString& title);
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
    QString outputDir() const;      // config Paths/RecordingFolder (falls back to DownloadFolder)
    bool    audioOnly() const;      // config Recording/Type == "Audio"
    QString extForFormat() const;   // config Recording/Format -> container ext for the active type
    QString mergeFormat() const;    // video clip container (mp4|mkv — yt-dlp can't merge to ts)
    static QString sanitize(const QString& name); // strip path-illegal chars
    static QStringList audioCodecArgs(const QString& ext); // -c:a … for an audio-only capture

    QProcess* m_proc = nullptr;
    QString   m_outFile;
    bool      m_stopping = false;

    // Hybrid clip pipeline. A clip first cuts the section DIRECTLY from the
    // already-resolved CDN URLs with ffmpeg (no yt-dlp launch — this writes the
    // final file). If CDN throttling drags ffmpeg's `speed=` below realtime, or the
    // grab stalls/fails (stale URL), we kill it and fall back to a concurrent full
    // yt-dlp download of the page URL followed by a local lossless ffmpeg trim.
    // m_clipProc is whichever stage is currently running.
    enum class ClipStage { None, Section, FullDownload, Trim };
    void startClipSection();
    void startClipFullDownload();
    void startClipTrim();
    void switchClipToFull();          // section throttled -> kill it, go full+trim
    void finishClip(const QString& file, bool ok); // single exit point: cleans up + emits
    static void killTree(QProcess* p);             // kill yt-dlp AND its ffmpeg child

    QProcess* m_clipProc = nullptr;   // active clip-stage process (yt-dlp or ffmpeg)
    ClipStage m_clipStage = ClipStage::None;
    QString   m_clipFile;             // final clip path
    QString   m_clipTempFile;         // full-download temp (deleted after the trim)
    QString   m_clipPageUrl, m_clipTitle;
    QUrl      m_clipVideoUrl, m_clipAudioUrl; // resolved CDN streams for the direct cut
    qint64    m_clipStartMs = 0, m_clipEndMs = 0;
    bool      m_clipYouTube = false;
    bool      m_clipAudio = false;    // Recording/Type at clip start (settings can change mid-clip)
    QElapsedTimer m_clipClock;        // section start — grace period before the speed watchdog
    int       m_clipSlowHits = 0;     // consecutive throttled section speed samples
    bool      m_clipSwitching = false;// section is being killed to switch to full download
    bool      m_clipCancelled = false;
};

#endif // SGRECORDER_H
