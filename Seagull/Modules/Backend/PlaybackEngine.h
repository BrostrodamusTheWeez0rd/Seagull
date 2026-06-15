#ifndef PLAYBACKENGINE_H
#define PLAYBACKENGINE_H

#include <QObject>
#include <QUrl>
#include <QString>
#include <QByteArray>
#include <QElapsedTimer>
#include <memory>
#include <vlcpp/vlc.hpp>

class AudioFifo;
class AudioSinkWorker;
class QThread;

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

    void play();   // no-op without loaded media (a stop must stay stopped)
    void pause();
    void stop();
    void releaseMedia(); // stop AND drop the media — full teardown for the Stop button
    bool isPlaying() const;

    qint64 time() const;
    qint64 length() const;
    void   setTime(qint64 ms);
    State  state() const;

    // Single-frame step (for paused frame-by-frame scrubbing). dir >= 0 advances
    // one frame (VLC's native next-frame, which keeps the player paused); dir < 0
    // steps back one frame's worth of time (VLC has no native previous-frame).
    void stepFrame(int dir);

    void setVolume(int volume);
    void setMute(bool muted);

    // Audio tap: route this player's decoded audio through our own QAudioSink so
    // we can both play it and analyse it (RMS level + beat onsets) to drive the
    // visualizer. ONLY used for audio-only media — video keeps VLC's native audio
    // output so its A/V sync is untouched. No-op + stays on VLC audio if the
    // output device can't be opened. Call before loading media.
    void setAudioTap(bool on);
    bool audioTapActive() const { return m_tapOn; }

signals:
    void endReached();
    void paused();
    void playing();
    void errorOccurred();

    // Emitted (on this thread, marshalled from VLC's audio thread) while the tap
    // is active: a normalised 0..1 level per audio buffer, a 3-band spectrum
    // (bass/mid/treble, computed from the PCM), and a pulse per beat.
    void audioLevel(float level);
    void audioSpectrum(float bass, float mid, float treble);
    void beat();

private:
    void createPlayer(); // (re)build the media player + hook its events
    void hookEvents();
    void onAudioData(const void* samples, unsigned count, int64_t pts); // VLC audio thread
    void ensureAudioThread(); // lazily create the dedicated audio output thread + worker
    void analyzeForVisualizer(const QByteArray& chunk); // sink pull thread: analyse consumed PCM
    void applyVolumeToSink();
    // Synthesise a local HLS master tying a video-only + audio-only chunklist
    // together (for sites that split them); returns false on write failure.
    bool writeHlsMaster(const QString& videoUrl, const QString& audioUrl);

    std::shared_ptr<VLC::Instance>    m_instance;
    std::shared_ptr<VLC::MediaPlayer> m_player;
    std::shared_ptr<VLC::Media>       m_lastMedia;   // kept for replay-from-start
    QString                           m_hlsMasterPath; // temp local master for split-HLS streams

    // Audio tap state.
    bool          m_tapOn       = false;
    QThread*        m_audioThread = nullptr; // dedicated output thread (immune to GUI freezes)
    AudioSinkWorker* m_audioWorker = nullptr; // owns the QAudioSink on m_audioThread
    AudioFifo*      m_fifo         = nullptr; // thread-safe PCM ring (plain, deleted manually)
    int           m_volume      = 100;     // cached so volume applies to whichever output is live
    bool          m_muted       = false;
    unsigned      m_tapRate     = 44100;
    unsigned      m_tapChannels = 2;
    double        m_energyEma   = 0.0;     // decaying energy average for beat detection
    qint64        m_lastBeatMs  = 0;
    QElapsedTimer m_beatClock;
    double        m_lpLow       = 0.0;     // one-pole filter states for the band split
    double        m_lpLow2      = 0.0;     // 2nd pole -> steeper bass/mid crossover
    double        m_lpMid       = 0.0;
    // Per-band auto-gain peak followers: each band is normalised to its own recent
    // peak so it never just pins at full (keeps headroom + movement on hot tracks).
    double        m_peakLevel   = 0.0;
    double        m_peakBass    = 0.0;
    double        m_peakMid     = 0.0;
    double        m_peakTreble  = 0.0;
};

#endif // PLAYBACKENGINE_H
