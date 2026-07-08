#include "PlaybackEngine.h"
#include "SgDynamics.h"
#include "SgEq.h"
#include <QObject>
#include <QFile>
#include <QDir>
#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QIODevice>
#include <QTimer>
#include <QMutex>
#include <QThread>
#include <functional>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <xmmintrin.h> // _MM_SET_FLUSH_ZERO_MODE  (FTZ: flush denormal results to zero)
#include <pmmintrin.h> // _MM_SET_DENORMALS_ZERO_MODE (DAZ: treat denormal inputs as zero)

namespace {
// Our float EQ/limiter biquads can produce denormal sample values (their IIR feedback
// decays toward zero). Arithmetic on denormals stalls the CPU ~100x; on the sink's pull
// thread that starves the buffer and crackles. FTZ+DAZ make the SSE unit treat denormals
// as zero — per-thread (MXCSR), so arm it at the top of every pull. x64 always has SSE2/3.
inline void armDenormalFlush() {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}
} // namespace

// Thread-safe PCM ring between VLC's audio thread (push) and the QAudioSink's
// pull thread (pop). Capped so a stalled consumer can't grow it unbounded.
class AudioFifo {
public:
    void clear() { QMutexLocker l(&m_mx); m_buf.clear(); }
    void push(const char* data, qint64 len) {
        QMutexLocker l(&m_mx);
        m_buf.append(data, len);
        // ~4s of stereo S16 (~1.4MB). Must comfortably exceed VLC's decode-ahead:
        // at track start VLC bursts decoded audio into the ring faster than realtime
        // to fill its own caches, and a tighter cap (it used to be ~1s — exactly
        // VLC's file-caching) overflows and evicts unplayed samples from the front,
        // an audible skip/glitch right as the track starts. The sink still plays in
        // order at the hardware rate, so the extra headroom adds no latency.
        const int cap = 44100 * 2 * 2 * 4;
        if (m_buf.size() > cap) m_buf.remove(0, m_buf.size() - cap);
    }
    QByteArray pop(qint64 maxlen) {
        QMutexLocker l(&m_mx);
        const int n = int(qMin<qint64>(maxlen, m_buf.size()));
        QByteArray out = m_buf.left(n);
        m_buf.remove(0, n);
        return out;
    }
    qint64 size() const { QMutexLocker l(&m_mx); return m_buf.size(); }
private:
    mutable QMutex m_mx;
    QByteArray m_buf;
};

// Pull-mode source for the QAudioSink. The sink reads this on its OWN audio
// thread, so audio keeps flowing even when the GUI event loop is frozen (e.g. the
// Windows modal window-move/resize loop) — which a GUI-timer-driven push mode
// could not survive. Underruns are zero-padded so the sink never idles on a brief
// gap (VLC decodes ahead on its own thread, so genuine underruns are rare).
class FifoDevice : public QIODevice {
public:
    FifoDevice(AudioFifo* fifo, std::function<void(const QByteArray&)> onConsumed,
               qint64 primeBytes, SgEq* eq, SgDynamics* dyn, int channels, QObject* parent = nullptr)
        : QIODevice(parent), m_fifo(fifo), m_onConsumed(std::move(onConsumed)),
          m_primeBytes(primeBytes), m_eq(eq), m_dyn(dyn), m_channels(channels) {}

protected:
    qint64 readData(char* data, qint64 maxlen) override {
        // The IIR EQ + limiter run in float here and can generate denormals; flush them
        // on whatever thread the sink pulls on (per-thread MXCSR) so they don't stall us.
        armDenormalFlush();
        // The FIFO holds VLC's clean S16; we do all DSP in a float scratch we generate
        // ourselves (no float ever crosses the VLC boundary — that path screams here),
        // then quantise back to S16 for the sink. data/maxlen are S16 units throughout.
        // Prime gate: until VLC has buffered a cushion, hand the sink silence
        // (consuming nothing) so the first real samples play out of a filled FIFO
        // instead of being spliced with zero-padding at the head — that splice is
        // the click/crackle heard at the start of a track. Self-re-arming (see the
        // dry-FIFO case below), so it kicks in for every track, not just the first.
        if (!m_primed) {
            if (m_fifo->size() < m_primeBytes) {
                std::memset(data, 0, size_t(maxlen));
                return maxlen;
            }
            m_primed = true;
        }
        QByteArray chunk = m_fifo->pop(maxlen);
        const qint64 got = chunk.size();
        if (got == 0) {
            // FIFO ran fully dry (track gap / genuine underrun): re-arm priming so
            // the next track / recovery rebuilds the cushion and resumes cleanly,
            // rather than splicing silence into the middle of live audio.
            m_primed = false;
            if (m_eq)  m_eq->reset();  // clear filter ringdown so it doesn't bleed across tracks
            if (m_dyn) m_dyn->reset(); // clear the gain ride so it doesn't bleed across tracks
            std::memset(data, 0, size_t(maxlen));
            return maxlen;
        }
        // Tap for the visualizer BEFORE our EQ + normaliser/limiter run: the
        // limiter's whole job is to flatten dynamics, so analyzing its output left
        // the visuals squashed. Same audio, same pull instant, dynamics intact.
        // (The analysis callback is synchronous, and the DSP below overwrites
        // chunk in place — so this must stay ahead of it.)
        if (m_onConsumed) m_onConsumed(chunk);
        const int ch = qMax(1, m_channels);
        const int frames = int(got / (2 * ch));
        const int nSamples = frames * ch;
        if (nSamples > 0) {
            // S16 -> float scratch (±1.0).
            if (int(m_scratch.size()) < nSamples) m_scratch.resize(nSamples);
            const int16_t* in = reinterpret_cast<const int16_t*>(chunk.constData());
            float* fb = m_scratch.data();
            for (int i = 0; i < nSamples; ++i) fb[i] = in[i] * (1.0f / 32768.0f);
            // Our own EQ first (its boost may overshoot 0 dBFS), THEN the normaliser +
            // brickwall limiter holds the result under the ceiling — so the S16 quantise
            // below can never clip. Both are bypassed (exact passthrough) when disabled.
            if (m_eq)  m_eq->process(fb, frames, ch);
            if (m_dyn) m_dyn->process(fb, frames, ch);
            // float -> S16 with a hard clamp guard (only bites if the limiter is off; it
            // CLAMPS, never wraps, so a stray over-range sample can't become harsh noise).
            int16_t* out = reinterpret_cast<int16_t*>(chunk.data()); // reuse chunk as S16 out
            for (int i = 0; i < nSamples; ++i) {
                const float v = qBound(-1.0f, fb[i], 1.0f);
                out[i] = int16_t(std::lround(v * 32767.0f));
            }
        }
        std::memcpy(data, chunk.constData(), size_t(got));
        if (got < maxlen) std::memset(data + got, 0, size_t(maxlen - got)); // pad a partial tail
        return maxlen; // always full so the sink stays Active across brief gaps
    }
    qint64 writeData(const char*, qint64) override { return -1; }

public:
    bool isSequential() const override { return true; }
    // The sink only pulls while it believes data is available. We always have
    // something to give (real PCM, or zero-padded silence in readData), so report
    // a steady non-zero amount or the sink would idle and play nothing.
    qint64 bytesAvailable() const override {
        return (qint64(1) << 16) + QIODevice::bytesAvailable();
    }

private:
    AudioFifo* m_fifo;
    std::function<void(const QByteArray&)> m_onConsumed;
    const qint64 m_primeBytes;   // FIFO fill required before the first real pull
    SgEq* m_eq = nullptr;        // graphic EQ (owned by the worker); null = bypass
    SgDynamics* m_dyn = nullptr; // normaliser + limiter (owned by the worker); null = bypass
    std::vector<float> m_scratch; // S16<->float work buffer, grown to the largest block
    int m_channels = 2;
    bool m_primed = false;
};

// Owns the QAudioSink on a DEDICATED thread, so the sink's pull runs on that
// thread and survives the GUI event loop freezing (Windows modal move/resize
// loop). Created on the GUI thread then moved to its thread; start/stop/setVolume
// are invoked queued so they execute there (and the sink/device are created there,
// giving them the right thread affinity). No Q_OBJECT needed — driven by functor
// QMetaObject::invokeMethod, not slots.
class AudioSinkWorker : public QObject {
public:
    AudioSinkWorker(AudioFifo* fifo, std::function<void(const QByteArray&)> onConsumed)
        : m_fifo(fifo), m_onConsumed(std::move(onConsumed)) {}
    ~AudioSinkWorker() override { stop(); }

    void start(int rate, int channels, int volume, bool muted, bool normEnabled,
               bool eqEnabled, const QVector<float>& eqGains, float eqPreamp) {
        stop();
        QAudioFormat fmt;
        fmt.setSampleRate(rate);
        fmt.setChannelCount(channels);
        fmt.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull() || !dev.isFormatSupported(fmt)) return;
        // Size the EQ + normaliser/limiter for this format and clear any prior state.
        m_eq.prepare(rate, channels);
        if (!eqGains.isEmpty()) m_eq.setParams(eqGains.constData(), eqGains.size(), eqPreamp);
        m_eq.setEnabled(eqEnabled);
        m_dyn.prepare(rate, channels);
        m_dyn.setEnabled(normEnabled);
        // Buffer a ~150ms cushion before emitting real audio so the track head plays
        // from a filled FIFO (no startup splice glitch); VLC decodes ahead, so the
        // cushion fills near-instantly and adds only a brief, inaudible head delay.
        m_device = new FifoDevice(m_fifo, m_onConsumed, fmt.bytesForDuration(150000), &m_eq, &m_dyn, channels);
        m_device->open(QIODevice::ReadOnly);
        m_sink = new QAudioSink(dev, fmt);
        m_sink->setBufferSize(fmt.bytesForDuration(100000)); // ~100ms: low latency, glitch-safe
        setVolume(volume, muted);
        m_sink->start(m_device); // pull mode, on this (audio) thread
    }
    void stop() {
        if (m_sink) { m_sink->stop(); delete m_sink; m_sink = nullptr; }
        if (m_device) { delete m_device; m_device = nullptr; }
    }
    void setVolume(int volume, bool muted) {
        if (m_sink) m_sink->setVolume(muted ? 0.0 : qBound(0, volume, 100) / 100.0);
    }
    // Halt/continue the pull WITHOUT draining or clearing the FIFO, so a pause stops
    // the sound the instant VLC pauses (instead of playing out the ~1s VLC decoded
    // ahead) and a resume picks up seamlessly from the buffered samples (no cutout).
    void suspend() { if (m_sink) m_sink->suspend(); }
    void resume()  { if (m_sink) m_sink->resume(); }
    // Live normalisation toggle — m_dyn's enable is atomic, so this is safe to call
    // from any thread (the GUI thread pokes it while the audio thread processes).
    void setNormalization(bool on) { m_dyn.setEnabled(on); }
    // Live EQ updates. setParams locks briefly; setEnabled is atomic. Invoked queued
    // onto this thread, so they're sequenced with start()/stop() too.
    void setEqEnabled(bool on) { m_eq.setEnabled(on); }
    void setEqParams(const QVector<float>& gains, float preamp) {
        if (!gains.isEmpty()) m_eq.setParams(gains.constData(), gains.size(), preamp);
    }

private:
    AudioFifo* m_fifo;
    std::function<void(const QByteArray&)> m_onConsumed;
    QAudioSink* m_sink = nullptr;
    QIODevice*  m_device = nullptr;
    SgEq        m_eq;    // our 10-band graphic EQ on the pull path (replaces libVLC's EQ here)
    SgDynamics  m_dyn;   // master-bus normaliser + brickwall limiter on the pull path
};

PlaybackEngine::PlaybackEngine(QObject* parent) : QObject(parent) {
    const char* vlcArgs[] = {
        "--no-mouse-events",
        "--no-keyboard-events",
        "--network-caching=300",
        "--file-caching=5000",
        // Reconnect dropped HTTP connections instead of going silent, and present
        // the same UA yt-dlp signed the CDN URLs with so the CDN doesn't throttle.
        "--http-reconnect",
        "--http-user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
    };

    m_instance = std::make_shared<VLC::Instance>(6, vlcArgs);
    createPlayer();
}

void PlaybackEngine::createPlayer() {
    m_player = std::make_shared<VLC::MediaPlayer>(*m_instance);
    m_player->setMouseInput(false);
    m_player->setKeyInput(false);
    hookEvents();
    applyEqualizerToPlayer(); // a rebuild (e.g. the audio-tap toggle) must not drop the EQ
}

// --- Equalizer (libVLC graphic EQ; VLC types stay inside this file) ----------
int PlaybackEngine::equalizerBandCount() {
    return static_cast<int>(VLC::Equalizer::bandCount());
}

float PlaybackEngine::equalizerBandFrequency(int band) {
    return VLC::Equalizer::bandFrequency(static_cast<unsigned>(band));
}

QStringList PlaybackEngine::equalizerPresetNames() {
    QStringList names;
    const unsigned n = VLC::Equalizer::presetCount();
    for (unsigned i = 0; i < n; ++i)
        names << QString::fromStdString(VLC::Equalizer::presetName(i));
    return names;
}

bool PlaybackEngine::equalizerPresetGains(int presetIndex, QVector<float>& gains, float& preampDb) {
    if (presetIndex < 0 || static_cast<unsigned>(presetIndex) >= VLC::Equalizer::presetCount())
        return false;
    VLC::Equalizer eq(static_cast<unsigned>(presetIndex)); // preset-loading ctor
    const unsigned bands = VLC::Equalizer::bandCount();
    gains.resize(static_cast<int>(bands));
    for (unsigned b = 0; b < bands; ++b)
        gains[static_cast<int>(b)] = eq.amp(b);
    preampDb = eq.preamp();
    return true;
}

void PlaybackEngine::setEqualizer(const QVector<float>& gains, float preampDb) {
    if (gains.size() != static_cast<int>(VLC::Equalizer::bandCount())) return; // wrong shape: ignore
    m_eqGains   = gains;
    m_eqPreamp  = preampDb;
    m_eqEnabled = true;
    applyEqualizerToPlayer();
}

void PlaybackEngine::disableEqualizer() {
    m_eqEnabled = false;
    if (m_player) m_player->unsetEqualizer();              // video / VLC-native path
    if (m_tapOn && m_audioWorker)                          // audio-tap path: bypass our SgEq live
        QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker]() { w->setEqEnabled(false); },
                                  Qt::QueuedConnection);
}


// libVLC's equalizer filter (used ONLY on the video / VLC-native output path now) has a
// large, fixed insertion loss: the instant it's engaged the signal drops ~10 dB beyond
// any preamp/band setting. We cancel it with a constant makeup folded into the preamp, so
// toggling the EQ changes tone, not loudness. Our own SgEq (the audio-tap path) has NO
// insertion loss — flat is exact unity — so it needs no makeup. Tune by ear: if the power
// button jumps the level on VIDEO, nudge this until on/off match (up = louder EQ-on).
static constexpr float kEqMakeupDb = 10.0f;

void PlaybackEngine::applyEqualizerToPlayer() {
    if (m_tapOn) {
        // Audio-tap path: WE do the EQ (SgEq on the pull thread), not libVLC — so the band
        // boost can be caught by our limiter before the S16 quantise instead of being
        // hard-clipped inside VLC. Keep libVLC's own EQ OFF here (it would clip upstream of
        // us) and push the current curve to the worker. No makeup (SgEq is unity at flat).
        if (m_player) m_player->unsetEqualizer();
        if (m_audioWorker) {
            const bool on = m_eqEnabled && !m_eqGains.isEmpty();
            const QVector<float> g = m_eqGains; const float p = m_eqPreamp;
            QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker, on, g, p]() {
                w->setEqParams(g, p);
                w->setEqEnabled(on);
            }, Qt::QueuedConnection);
        }
        return;
    }
    // Video / VLC-native path: use libVLC's equalizer (with the insertion-loss makeup).
    if (!m_player || !m_eqEnabled || m_eqGains.isEmpty()) return;
    VLC::Equalizer eq;                 // default ctor: all bands zeroed
    // +makeup cancels the filter's insertion loss; clamp to VLC's ±20 dB preamp range.
    eq.setPreamp(qBound(-20.0f, m_eqPreamp + kEqMakeupDb, 20.0f));
    const unsigned bands = VLC::Equalizer::bandCount();
    for (unsigned b = 0; b < bands && static_cast<int>(b) < m_eqGains.size(); ++b)
        eq.setAmp(m_eqGains[static_cast<int>(b)], b);
    m_player->setEqualizer(eq);        // VLC keeps no ref; safe to let eq die after
}

void PlaybackEngine::setTapAudioDelayMs(int ms) {
    // A/V sync trim for the tap path. Our QAudioSink buffers audio (FIFO cushion + sink
    // buffer), so when we tap a VIDEO's audio, VLC — which thinks amem consumes instantly —
    // renders video ahead of the sound by roughly that buffering. A negative VLC audio
    // delay advances the audio in VLC's timeline to cancel it (0 for audio-only, where
    // there's no video to sync to). VLC clears the delay on each media change, so we also
    // re-apply it from the onPlaying handler. Tunable by ear (the right value is ~ the
    // sink's end-to-end latency).
    m_tapAudioDelayMs = ms;
    if (m_player && m_tapOn) m_player->setAudioDelay(qint64(ms) * 1000); // micros
}

PlaybackEngine::~PlaybackEngine() {
    if (m_player) m_player->stop(); // stop VLC first -> no more onAudioData into the ring
    if (m_audioThread) {
        // Stop the sink on its own thread and wait, then tear the thread down, so
        // no pull (and thus no analyzeForVisualizer callback into us) can run after.
        if (m_audioWorker)
            QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker]() { w->stop(); },
                                      Qt::BlockingQueuedConnection);
        m_audioThread->quit();
        m_audioThread->wait();
        delete m_audioWorker; m_audioWorker = nullptr;
        delete m_audioThread; m_audioThread = nullptr;
    }
    delete m_fifo; m_fifo = nullptr;
    if (!m_hlsMasterPath.isEmpty()) QFile::remove(m_hlsMasterPath);
}

void PlaybackEngine::hookEvents() {
    // VLC fires these on its own thread; marshal onto this object's (UI) thread
    // so connected slots run there, exactly like the old MainWindow handlers did.
    m_player->eventManager().onEndReached([this]() {
        QMetaObject::invokeMethod(this, [this]() { emit endReached(); }, Qt::QueuedConnection);
        });
    m_player->eventManager().onPaused([this]() {
        // Tap path: freeze the sink the moment VLC pauses so pause is instant and the
        // buffered PCM survives for a clean resume (see AudioSinkWorker::suspend).
        if (m_tapOn && m_audioWorker)
            QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker]() { w->suspend(); },
                                      Qt::QueuedConnection);
        QMetaObject::invokeMethod(this, [this]() { emit paused(); }, Qt::QueuedConnection);
        });
    m_player->eventManager().onPlaying([this]() {
        if (m_tapOn && m_audioWorker)
            QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker]() { w->resume(); },
                                      Qt::QueuedConnection);
        // VLC resets audio delay on each media change; re-assert our tap A/V-sync trim
        // now the input exists (no-op when it's 0, i.e. audio-only).
        if (m_tapOn && m_player && m_tapAudioDelayMs != 0)
            m_player->setAudioDelay(qint64(m_tapAudioDelayMs) * 1000);
        QMetaObject::invokeMethod(this, [this]() { emit playing(); }, Qt::QueuedConnection);
        });
    m_player->eventManager().onEncounteredError([this]() {
        QMetaObject::invokeMethod(this, [this]() { emit errorOccurred(); }, Qt::QueuedConnection);
        });
}

void PlaybackEngine::setOutputWindow(void* hwnd) {
    if (!m_player) return;
    m_player->setHwnd(hwnd);
    m_player->setMouseInput(false);
    m_player->setKeyInput(false);
}

void PlaybackEngine::loadLocalFile(const QString& path, qint64 startMs) {
    m_loadPath = path; // remembered so replay can rebuild a fresh Media (see reloadLastMedia)
    m_lastMedia = std::make_shared<VLC::Media>(*m_instance, path.toUtf8().constData(), VLC::Media::FromPath);
    // Resume support: seek straight to a saved position on load (watch history).
    // Same mechanism the stream path and the video-normalization reload use.
    if (startMs > 0)
        m_lastMedia->addOption(QString(":start-time=%1").arg(startMs / 1000.0).toUtf8().constData());
    // Local disk doesn't need the 5s instance-level file-caching (that's for network
    // streams). An oversized cache lets the decoder front-run the output clock on a
    // restart, widening the "playback way too early -> inserting zeroes" silence pad
    // that drops audio at the head of a replay. VLC's own default is 1000ms.
    m_lastMedia->addOption(":file-caching=1000");
    // AV1 must decode in software (dav1d): VLC 3's AV1-over-D3D11VA hwaccel has
    // a broken frame pool (get_buffer()/"Failed to allocate space for current
    // frame" spam), which plays as constant stutter and a big hitch on
    // pause/unpause. dav1d claims ONLY AV1, so every other codec falls through
    // to "any" and keeps hardware decoding.
    m_lastMedia->addOption(":codec=dav1d,any");
    addVideoNormalizationOptions(*m_lastMedia); // compressor-as-limiter when video norm is on
    m_player->setMedia(*m_lastMedia);
    applyEqualizerToPlayer(); // re-assert EQ for the new media (before play())
}

void PlaybackEngine::loadStream(const QUrl& videoUrl, const QUrl& audioUrl, qint64 startMs,
    const QString& referer) {
    m_loadPath.clear(); // a stream: replay re-arms m_lastMedia, not a local rebuild
    const bool hasAudio = audioUrl.isValid() && !audioUrl.isEmpty();
    // HLS sites that split audio into its own playlist (e.g. Chaturbate): VLC won't
    // pull audio from an input-slave .m3u8, and the site's own master manifest is
    // single-use (yt-dlp burns its token during extraction), so we synthesise a tiny
    // local master that points at the reusable video + audio chunklists and let VLC's
    // native adaptive demuxer combine them. avformat (below) is reserved for DASH.
    const bool isHls = hasAudio &&
        (videoUrl.toString().contains(".m3u8", Qt::CaseInsensitive)
            || audioUrl.toString().contains(".m3u8", Qt::CaseInsensitive));

    QString mediaLocation = videoUrl.toString();
    bool fromPath = false;

    if (isHls && writeHlsMaster(videoUrl.toString(), audioUrl.toString())) {
        mediaLocation = QDir::toNativeSeparators(m_hlsMasterPath);
        fromPath = true;
    }

    m_lastMedia = std::make_shared<VLC::Media>(*m_instance, mediaLocation.toUtf8().constData(),
        fromPath ? VLC::Media::FromPath : VLC::Media::FromLocation);

    m_lastMedia->addOption(":network-caching=300");
    m_lastMedia->addOption(":http-reconnect=true");      // auto-reconnect dropped HTTP
    m_lastMedia->addOption(":adaptive-logic=highest");   // top rendition for native HLS / DASH
    m_lastMedia->addOption(":codec=dav1d,any");          // AV1 in software — see loadLocalFile

    // Many CDNs (PornHub, xvideos, ...) hotlink-protect their streams: VLC must
    // send the page URL as Referer, like yt-dlp does, or the request is rejected.
    // Harmless for YouTube (signed googlevideo URLs ignore it).
    if (!referer.isEmpty())
        m_lastMedia->addOption(QString(":http-referrer=" + referer).toUtf8().constData());

    // YouTube serves separate adaptive video+audio as fragmented mp4 (DASH):
    // avformat demuxes it and input-slave merges the audio. We DON'T use avformat
    // for HLS — on Windows it mangles segment URLs into \\host\path UNC paths and
    // can't open them — so HLS pairs go through the synthesised master above instead.
    if (hasAudio && !isHls) {
        m_lastMedia->addOption(":demux=avformat");
        m_lastMedia->addOption(QString(":input-slave=" + audioUrl.toString()).toUtf8().constData());
    }

    if (startMs > 0)
        m_lastMedia->addOption(QString(":start-time=%1").arg(startMs / 1000.0).toUtf8().constData());

    addVideoNormalizationOptions(*m_lastMedia); // compressor-as-limiter when video norm is on
    m_player->setMedia(*m_lastMedia);
    applyEqualizerToPlayer(); // re-assert EQ for the new media (before play())
}

// Writes a minimal HLS master playlist that ties a video-only chunklist to an
// audio-only chunklist via an EXT-X-MEDIA audio group, so VLC's adaptive demuxer
// plays both. Returns false (caller falls back to the bare video URL) on failure.
bool PlaybackEngine::writeHlsMaster(const QString& videoUrl, const QString& audioUrl) {
    const QString master =
        "#EXTM3U\n#EXT-X-VERSION:6\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\""
        + audioUrl + "\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4000000,CODECS=\"avc1.4d401f,mp4a.40.2\",AUDIO=\"aud\"\n"
        + videoUrl + "\n";

    if (m_hlsMasterPath.isEmpty())
        m_hlsMasterPath = QDir::tempPath()
        + QString("/seagull_hls_%1.m3u8").arg(reinterpret_cast<quintptr>(this));

    QFile f(m_hlsMasterPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(master.toUtf8());
    f.close();
    return true;
}

void PlaybackEngine::reloadLastMedia() {
    if (!m_player) return;
    m_player->stop();
    // Local files: rebuild a fresh Media rather than re-arming the one that just
    // played to EOF. Reusing the played-out object can leave VLC's restart clock
    // thinking playback already advanced, so it pads silence at the head ("playback
    // way too early ... inserting zeroes") — an audible dropout on replay. A fresh
    // Media (with its file-caching option) gives the input a clean start.
    if (!m_loadPath.isEmpty()) {
        loadLocalFile(m_loadPath); // sets m_lastMedia fresh + re-applies EQ
        return;
    }
    if (!m_lastMedia) return;
    m_player->setMedia(*m_lastMedia);
    applyEqualizerToPlayer(); // keep EQ across replay / shorts-loop reloads
}

bool PlaybackEngine::hasMedia() const { return m_lastMedia != nullptr; }

void PlaybackEngine::releaseMedia() {
    // Full teardown (Stop button): without this, the media stays loaded in VLC
    // and a later play() restarts it from nowhere (and a local file stays held).
    stop();
    m_lastMedia.reset();
    m_loadPath.clear();
}

// play() requires loaded media: after releaseMedia a stray play() (space bar,
// stale controls) must not resurrect whatever VLC still has internally.
void PlaybackEngine::play() { if (m_player && m_lastMedia) m_player->play(); }
void PlaybackEngine::pause() { if (m_player) m_player->pause(); }
void PlaybackEngine::stop() { if (m_player) m_player->stop(); }
bool PlaybackEngine::isPlaying() const { return m_player && m_player->isPlaying(); }

qint64 PlaybackEngine::time() const { return m_player ? m_player->time() : 0; }
qint64 PlaybackEngine::length() const { return m_player ? m_player->length() : 0; }
void PlaybackEngine::setTime(qint64 ms) { if (m_player) m_player->setTime(ms); }

void PlaybackEngine::stepFrame(int dir) {
    if (!m_player) return;
    if (dir >= 0) {
        m_player->nextFrame(); // advances one frame and holds the player paused
        return;
    }
    // No native previous-frame in VLC: step back by one frame's duration, derived
    // from the current FPS (fall back to ~25fps if VLC can't report it yet).
    // vlcpp only wraps fps() pre-3.0, so call the (still-exported) C API directly.
    const float f = libvlc_media_player_get_fps(*m_player);
    const qint64 frameMs = (f > 1.0f) ? qint64(1000.0f / f + 0.5f) : 40;
    m_player->setTime(qMax<qint64>(0, m_player->time() - frameMs));
}

PlaybackEngine::State PlaybackEngine::state() const {
    if (!m_player) return State::Idle;
    switch (m_player->state()) {
    case libvlc_Opening:   return State::Opening;
    case libvlc_Buffering: return State::Buffering;
    case libvlc_Playing:   return State::Playing;
    case libvlc_Paused:    return State::Paused;
    case libvlc_Stopped:   return State::Stopped;
    case libvlc_Ended:     return State::Ended;
    case libvlc_Error:     return State::Error;
    default:               return State::Idle;
    }
}

void PlaybackEngine::setVolume(int volume) {
    m_volume = volume;
    if (m_tapOn) applyVolumeToSink();          // our sink owns volume while tapping
    else if (m_player) m_player->setVolume(volume);
}
void PlaybackEngine::setMute(bool muted) {
    m_muted = muted;
    if (m_tapOn) applyVolumeToSink();
    else if (m_player) m_player->setMute(muted);
}

void PlaybackEngine::applyVolumeToSink() {
    if (!m_audioWorker) return;
    const int vol = m_volume; const bool mu = m_muted;
    QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker, vol, mu]() { w->setVolume(vol, mu); },
                              Qt::QueuedConnection);
}

// --- Normalization / peak protection ----------------------------------------

void PlaybackEngine::setAudioNormalizationEnabled(bool on) {
    m_normAudio = on;
    // Live on the audio tap when audio is the playing path; otherwise the state is
    // remembered and applied when the sink next starts (see setAudioTap).
    if (m_tapOn && m_audioWorker)
        QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker, on]() { w->setNormalization(on); },
                                  Qt::QueuedConnection);
}

void PlaybackEngine::setVideoNormalizationEnabled(bool on) {
    // Pure state set: the compressor binds as a media option at load time, so this just
    // records the desired state for the next loadLocalFile/loadStream. A live toggle of
    // a currently-playing video goes through reloadForVideoNormalization().
    m_normVideo = on;
}

void PlaybackEngine::reloadForVideoNormalization() {
    // For a LOCAL video playing right now, reload in place so a normalization toggle is
    // audible immediately; streams pick it up on their next load (re-fetching a stream
    // just to toggle a filter risks burning a single-use token, so we don't).
    if (m_tapOn || m_loadPath.isEmpty() || !m_lastMedia) return;
    const qint64 t = time();
    const bool wasPlaying = isPlaying();
    loadLocalFile(m_loadPath); // fresh Media -> adds/drops the compressor option per m_normVideo
    if (t > 0)
        m_lastMedia->addOption(QString(":start-time=%1").arg(t / 1000.0).toUtf8().constData());
    if (wasPlaying) play();
}

void PlaybackEngine::addVideoNormalizationOptions(VLC::Media& media) const {
    // Video path only: audio-only media uses our own SgDynamics on the sink instead.
    if (!m_normVideo || m_tapOn) return;
    // libVLC's compressor audio filter, tuned as a brickwall-ish limiter so video audio
    // can't clip: peak sensing, threshold just under 0 dB, max ratio, fast attack, unity
    // makeup. Bound per-input so it never reaches the audio-tap path. Volume is applied
    // by the aout core AFTER this filter, so the user's fader still scales the result.
    media.addOption(":audio-filter=compressor");
    media.addOption(":compressor-rms-peak=0.0");
    media.addOption(":compressor-attack=1.5");
    media.addOption(":compressor-release=120.0");
    media.addOption(":compressor-threshold=-1.5");
    media.addOption(":compressor-ratio=20.0");
    media.addOption(":compressor-knee=1.0");
    media.addOption(":compressor-makeup-gain=0.0");
}

// --- Audio tap (audio-only media) -------------------------------------------

void PlaybackEngine::ensureAudioThread() {
    if (m_audioThread) return;
    if (!m_fifo) m_fifo = new AudioFifo();
    m_audioThread = new QThread();
    m_audioThread->setObjectName("SeagullAudioOut");
    // Analysis runs on the audio thread (in the pull) and marshals its emit to the
    // GUI thread itself (see analyzeForVisualizer).
    m_audioWorker = new AudioSinkWorker(m_fifo, [this](const QByteArray& c) { analyzeForVisualizer(c); });
    m_audioWorker->moveToThread(m_audioThread);
    m_audioThread->start(QThread::HighPriority);
}

void PlaybackEngine::setAudioTap(bool on) {
    if (on == m_tapOn) return;
    if (on) {
        // Device-support check on this (GUI) thread so we can fall back to VLC's
        // native audio if there's no usable output, before committing to the tap.
        QAudioFormat fmt;
        fmt.setSampleRate(int(m_tapRate));
        fmt.setChannelCount(int(m_tapChannels));
        fmt.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull() || !dev.isFormatSupported(fmt)) return; // stay on VLC's audio output

        m_tapOn = true;
        m_energyEma = 0.0;
        m_lastBeatMs = 0;
        m_lpLow = m_lpLow2 = m_lpMidLo = m_lpMidLo2 = m_lpMid = m_lpMid2 = 0.0;
        m_peakLevel = m_peakBass = m_peakMid = m_peakTreble = 0.0;
        m_beatClock.restart();

        if (!m_fifo) m_fifo = new AudioFifo();
        m_fifo->clear();
        ensureAudioThread();

        // Custom decoded-audio output: VLC hands us S16 interleaved samples; the
        // worker thread's sink plays them, we analyse them. flush() clears the ring
        // on seek so stale audio isn't replayed.
        m_player->setAudioCallbacks(
            [this](const void* s, unsigned c, int64_t pts) { onAudioData(s, c, pts); },
            nullptr, nullptr,
            [this](int64_t) { if (m_fifo) m_fifo->clear(); },
            nullptr);
        m_player->setAudioFormat("S16N", m_tapRate, m_tapChannels);

        // Start the sink on the audio thread (it owns/creates the sink there). Hand it the
        // current EQ state too, so our graphic EQ is active from the first block of audio
        // (the tap does the EQ now, not libVLC — see applyEqualizerToPlayer).
        const int rate = int(m_tapRate), ch = int(m_tapChannels), vol = m_volume;
        const bool mu = m_muted; const bool norm = m_normAudio;
        const bool eqOn = m_eqEnabled; const QVector<float> eqG = m_eqGains; const float eqP = m_eqPreamp;
        QMetaObject::invokeMethod(m_audioWorker,
            [w = m_audioWorker, rate, ch, vol, mu, norm, eqOn, eqG, eqP]() {
                w->start(rate, ch, vol, mu, norm, eqOn, eqG, eqP);
            }, Qt::QueuedConnection);
    } else {
        m_tapOn = false;
        if (m_audioWorker)
            QMetaObject::invokeMethod(m_audioWorker, [w = m_audioWorker]() { w->stop(); },
                                      Qt::QueuedConnection);
        // libVLC can't cleanly switch a player off its callback (amem) audio
        // output, so rebuild the player to restore normal audio. The caller
        // rebinds the video window and loads media immediately after.
        createPlayer();
        m_player->setVolume(m_volume);
        m_player->setMute(m_muted);
    }
}

// Runs on the sink's pull thread, on the exact PCM being sent to the sink — so the
// visualizer tracks what's HEARD, not what VLC has decoded ahead (VLC front-loads
// the ring). The only remaining lead is the sink's own buffer, held back by
// kVisualDelayMs. The compute is light (per-sample IIR); the emit is marshalled to
// the GUI thread, so during a UI freeze the visualizer pauses but audio does not.
void PlaybackEngine::analyzeForVisualizer(const QByteArray& chunk) {
    const int ch = qMax(1, int(m_tapChannels));
    const qint64 frames = (chunk.size() / 2) / ch;
    if (frames <= 0) return;
    const int16_t* s = reinterpret_cast<const int16_t*>(chunk.constData());

    // Bass keeps its ~330Hz 12dB/oct lowpass. Mid gets its OWN steep low edge at
    // ~600Hz (2 poles): subtracting only the 330Hz lowpass left the kick's body
    // and harmonics inside the mid band, which made mid visually mirror bass —
    // the two bands read as one. Mid/treble split ~1.4kHz (1-pole; lowered from
    // 2kHz so vocal presence and consonants drive the treble sheet too).
    const double aLow = 0.046, aMidLo = 0.085, aMid = 0.180;
    double sumsq = 0.0, eBass = 0.0, eMid = 0.0, eTreb = 0.0;
    for (qint64 f = 0; f < frames; ++f) {
        const double x = (ch >= 2) ? (s[f * ch] + s[f * ch + 1]) * (0.5 / 32768.0)
                                   : s[f * ch] / 32768.0;
        sumsq += x * x;
        m_lpLow    += aLow   * (x - m_lpLow);
        m_lpLow2   += aLow   * (m_lpLow - m_lpLow2);    // second pole -> steeper bass edge
        m_lpMidLo  += aMidLo * (x - m_lpMidLo);
        m_lpMidLo2 += aMidLo * (m_lpMidLo - m_lpMidLo2); // second pole -> steep mid low edge
        m_lpMid    += aMid   * (x - m_lpMid);
        m_lpMid2   += aMid   * (m_lpMid - m_lpMid2);    // second pole -> steep mid/treble split:
                                                        // kick clicks stay OUT of treble
        const double bass = m_lpLow2;                   // well-defined low band
        const double mid  = m_lpMid2 - m_lpMidLo2;      // ~600Hz..1.4kHz, both edges steep
        const double treb = x - m_lpMid2;
        eBass += bass * bass; eMid += mid * mid; eTreb += treb * treb;
    }
    const double inv = 1.0 / double(frames);

    // Auto-gain: normalise each band to its OWN recent peak (fast attack, slow
    // decay) so it maps the track's dynamics into 0..1 and never just pins at full
    // on hot/bass-heavy material — there's always headroom and movement left.
    // Then an expansion stage: dense modern material spends its life at 0.6-0.9 of
    // its own peak, which reads as near-static on screen, so we stretch that band
    // across the full range (an upward expander, the inverse of what a limiter
    // does) and smoothstep it for soft knees at silence and full-scale.
    auto agc = [](double raw, double& peak) -> float {
        constexpr double decay  = 0.9990; // peak falls slowly (~seconds) -> stable auto-range
        constexpr double floorv = 0.03;   // low enough that quiet intros still register
        peak = std::max(raw, peak * decay);
        double v = qBound(0.0, raw / std::max(peak, floorv), 1.0);
        v = qBound(0.0, (v - 0.12) / 0.78, 1.0);  // 0.12..0.90 of peak -> full swing
        return float(v * v * (3.0 - 2.0 * v));
    };
    const float level  = agc(std::sqrt(sumsq * inv), m_peakLevel);
    const float bass   = agc(std::sqrt(eBass * inv), m_peakBass);
    const float mid    = agc(std::sqrt(eMid  * inv), m_peakMid);
    const float treble = agc(std::sqrt(eTreb * inv), m_peakTreble);

    const double bassMeanSq = eBass * inv;
    bool isBeat = false;
    if (m_energyEma <= 0.0) m_energyEma = bassMeanSq;
    const qint64 nowMs = m_beatClock.elapsed();
    if (bassMeanSq > m_energyEma * 1.4 && bassMeanSq > 1e-4 && (nowMs - m_lastBeatMs) > 180) {
        isBeat = true; m_lastBeatMs = nowMs;
    }
    m_energyEma = m_energyEma * 0.92 + bassMeanSq * 0.08;

    constexpr int kVisualDelayMs = 90; // ~ the sink buffer, so the visual lands when heard
    // We're on the sink's pull thread; hop to the GUI thread before touching timers
    // or emitting to the (GUI) visualizer.
    QMetaObject::invokeMethod(this, [this, level, bass, mid, treble, isBeat]() {
        QTimer::singleShot(kVisualDelayMs, this, [this, level, bass, mid, treble, isBeat]() {
            emit audioLevel(level);
            emit audioSpectrum(bass, mid, treble);
            if (isBeat) emit beat();
        });
    }, Qt::QueuedConnection);
}

void PlaybackEngine::onAudioData(const void* samples, unsigned count, int64_t /*pts*/) {
    // VLC's audio thread: just hand the PCM to the ring. Analysis happens at
    // CONSUMPTION time in analyzeForVisualizer() (called from the sink's pull
    // device) so the visualizer stays in sync with what's actually being heard.
    if (!m_fifo) return;
    const qint64 nSamples = qint64(count) * m_tapChannels;
    m_fifo->push(static_cast<const char*>(samples), nSamples * 2); // S16 = 2 bytes
}
