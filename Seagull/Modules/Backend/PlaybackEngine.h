#ifndef PLAYBACKENGINE_H
#define PLAYBACKENGINE_H

#include <QObject>
#include <QUrl>
#include <QString>
#include <QStringList>
#include <QVector>
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
    void loadLocalFile(const QString& path, qint64 startMs = 0);
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
    // we can play it, run our own EQ/limiter chain on it (SgEq -> SgDynamics), and
    // analyse it (RMS level + beat onsets) to drive the visualizer. Used for ALL
    // audio-bearing media — audio and video alike (photos stay untapped); a tapped
    // video's sink-buffer latency is trimmed via setTapAudioDelayMs. No-op + stays
    // on VLC audio if the output device can't be opened. Call before loading media.
    void setAudioTap(bool on);
    bool audioTapActive() const { return m_tapOn; }

    // A/V sync trim (milliseconds) for the tap path: compensates the sink's buffering so a
    // tapped VIDEO's audio stays lip-synced. Negative = advance audio. 0 for audio-only.
    // Re-applied automatically on each play (VLC clears audio delay per media change).
    void setTapAudioDelayMs(int ms);

    // --- Equalizer (neutral API; no vlcpp type leaks out) ---------------------
    // gains: one dB value per band (size must equal equalizerBandCount(), else the
    // call is ignored). gain/preamp clamp to ±20 dB inside VLC. Applies live + to
    // subsequent media, and is re-applied automatically after the player is rebuilt
    // (audio-tap toggle) and after each setMedia, so it survives both.
    void setEqualizer(const QVector<float>& gains, float preampDb);
    void disableEqualizer();
    bool equalizerEnabled() const { return m_eqEnabled; }

    // --- Output normalization / peak protection -------------------------------
    // Per-kind, mirroring the EQ. Audio-only media runs through our own look-ahead
    // brickwall limiter + loudness normaliser on the sink (SgDynamics, applied live).
    // Video runs through libVLC's compressor audio filter (configured as a limiter),
    // bound as a media option at load — so the Video toggle takes effect on the next
    // video load (a currently-playing LOCAL video is reloaded in place; streams apply
    // on next load). The user's volume is applied after both, so it always wins.
    void setAudioNormalizationEnabled(bool on);
    void setVideoNormalizationEnabled(bool on);  // pure set; takes effect on next video load
    void reloadForVideoNormalization();          // live-reload a local video to apply it now
    bool audioNormalizationEnabled() const { return m_normAudio; }
    bool videoNormalizationEnabled() const { return m_normVideo; }

    // Static descriptors for building the UI (band labels) + VLC's built-in presets.
    static int          equalizerBandCount();
    static float        equalizerBandFrequency(int band);   // Hz
    static QStringList  equalizerPresetNames();
    static bool         equalizerPresetGains(int presetIndex, QVector<float>& gains, float& preampDb);

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
    QString                           m_loadPath;    // last local file path; empty for streams (replay rebuilds a fresh Media)
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
    int           m_tapAudioDelayMs = 0; // A/V sync trim for a tapped video (see setTapAudioDelayMs)
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

    // Equalizer state, kept here so it can be re-applied after the player is rebuilt
    // (setAudioTap toggle) and after each setMedia. m_eqGains empty => never set.
    void           applyEqualizerToPlayer();
    QVector<float> m_eqGains;
    float          m_eqPreamp  = 0.0f;
    bool           m_eqEnabled = false;

    // Normalization state. Audio is forwarded live to the sink worker; video is added
    // as compressor media options at load (see addVideoNormalizationOptions).
    void addVideoNormalizationOptions(VLC::Media& media) const; // no-op unless m_normVideo
    bool m_normAudio = false;
    bool m_normVideo = false;
};

#endif // PLAYBACKENGINE_H
