#ifndef PLAYBACKENGINE_H
#define PLAYBACKENGINE_H

#include <QObject>
#include <QUrl>
#include <QString>
#include <memory>
#include <vlcpp/vlc.hpp>

// Wraps libVLC so the rest of the app never touches vlcpp directly. Owns the VLC
// instance + media player, exposes a neutral transport API, and turns VLC's
// (off-thread) callbacks into Qt signals delivered on this object's thread. The
// video surface is bound once, by native handle, via setOutputWindow().
class PlaybackEngine : public QObject {
    Q_OBJECT
public:
    // Neutral playback state — mapped from libvlc_state_t so no VLC type leaks out.
    enum class State { Idle, Opening, Buffering, Playing, Paused, Stopped, Ended, Error };

    explicit PlaybackEngine(QObject* parent = nullptr);
    ~PlaybackEngine() override;

    void setOutputWindow(void* hwnd); // bind VLC's render target to a native window

    // Load methods set the media but don't start playback, so callers keep the
    // small "settle then play" delay the UI relies on. Call play() after.
    void loadLocalFile(const QString& path);
    void loadStream(const QUrl& videoUrl, const QUrl& audioUrl, qint64 startMs,
        const QString& referer = QString());
    void reloadLastMedia();           // stop + re-arm the last media (for replay)
    bool hasMedia() const;

    void play();
    void pause();
    void stop();
    bool isPlaying() const;

    qint64 time() const;
    qint64 length() const;
    void   setTime(qint64 ms);
    State  state() const;

    void setVolume(int volume);
    void setMute(bool muted);

signals:
    void endReached();
    void paused();
    void playing();
    void errorOccurred();

private:
    void hookEvents();
    // Synthesise a local HLS master tying a video-only + audio-only chunklist
    // together (for sites that split them); returns false on write failure.
    bool writeHlsMaster(const QString& videoUrl, const QString& audioUrl);

    std::shared_ptr<VLC::Instance>    m_instance;
    std::shared_ptr<VLC::MediaPlayer> m_player;
    std::shared_ptr<VLC::Media>       m_lastMedia;   // kept for replay-from-start
    QString                           m_hlsMasterPath; // temp local master for split-HLS streams
};

#endif // PLAYBACKENGINE_H
