#include "PlaybackEngine.h"
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
#include <cmath>
#include <cstring>
#include <cstdint>

// Thread-safe PCM ring between VLC's audio thread (push) and the GUI thread
// (pop, which writes to the QAudioSink in push mode). Capped so a stalled sink
// can't grow it unbounded.
class AudioFifo {
public:
    void clear() { QMutexLocker l(&m_mx); m_buf.clear(); }
    void push(const char* data, qint64 len) {
        QMutexLocker l(&m_mx);
        m_buf.append(data, len);
        const int cap = 44100 * 2 * 2; // ~1s of stereo S16
        if (m_buf.size() > cap) m_buf.remove(0, m_buf.size() - cap);
    }
    QByteArray pop(qint64 maxlen) {
        QMutexLocker l(&m_mx);
        const int n = int(qMin<qint64>(maxlen, m_buf.size()));
        QByteArray out = m_buf.left(n);
        m_buf.remove(0, n);
        return out;
    }
private:
    mutable QMutex m_mx;
    QByteArray m_buf;
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
}

PlaybackEngine::~PlaybackEngine() {
    if (m_player) m_player->stop();
    stopTapSink();
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
        QMetaObject::invokeMethod(this, [this]() { emit paused(); }, Qt::QueuedConnection);
        });
    m_player->eventManager().onPlaying([this]() {
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

void PlaybackEngine::loadLocalFile(const QString& path) {
    m_lastMedia = std::make_shared<VLC::Media>(*m_instance, path.toUtf8().constData(), VLC::Media::FromPath);
    // AV1 must decode in software (dav1d): VLC 3's AV1-over-D3D11VA hwaccel has
    // a broken frame pool (get_buffer()/"Failed to allocate space for current
    // frame" spam), which plays as constant stutter and a big hitch on
    // pause/unpause. dav1d claims ONLY AV1, so every other codec falls through
    // to "any" and keeps hardware decoding.
    m_lastMedia->addOption(":codec=dav1d,any");
    m_player->setMedia(*m_lastMedia);
}

void PlaybackEngine::loadStream(const QUrl& videoUrl, const QUrl& audioUrl, qint64 startMs,
    const QString& referer) {
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

    m_player->setMedia(*m_lastMedia);
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
    if (!m_player || !m_lastMedia) return;
    m_player->stop();
    m_player->setMedia(*m_lastMedia);
}

bool PlaybackEngine::hasMedia() const { return m_lastMedia != nullptr; }

void PlaybackEngine::releaseMedia() {
    // Full teardown (Stop button): without this, the media stays loaded in VLC
    // and a later play() restarts it from nowhere (and a local file stays held).
    stop();
    m_lastMedia.reset();
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
    if (m_sink) m_sink->setVolume(m_muted ? 0.0 : qBound(0, m_volume, 100) / 100.0);
}

// --- Audio tap (audio-only media) -------------------------------------------

void PlaybackEngine::setAudioTap(bool on) {
    if (on == m_tapOn) return;
    if (on) {
        if (!startTapSink()) return; // device unavailable: stay on VLC's audio output
        m_tapOn = true;
        m_energyEma = 0.0;
        m_lastBeatMs = 0;
        m_lpLow = m_lpLow2 = m_lpMid = 0.0;
        m_peakLevel = m_peakBass = m_peakMid = m_peakTreble = 0.0;
        m_beatClock.restart();
        // Custom decoded-audio output: VLC hands us S16 interleaved samples; we
        // play + analyse them. flush() clears the ring on seek so stale audio
        // isn't replayed.
        m_player->setAudioCallbacks(
            [this](const void* s, unsigned c, int64_t pts) { onAudioData(s, c, pts); },
            nullptr, nullptr,
            [this](int64_t) { if (m_fifo) m_fifo->clear(); },
            nullptr);
        m_player->setAudioFormat("S16N", m_tapRate, m_tapChannels);
        applyVolumeToSink();
    } else {
        m_tapOn = false;
        stopTapSink();
        // libVLC can't cleanly switch a player off its callback (amem) audio
        // output, so rebuild the player to restore normal audio. The caller
        // rebinds the video window and loads media immediately after.
        createPlayer();
        m_player->setVolume(m_volume);
        m_player->setMute(m_muted);
    }
}

bool PlaybackEngine::startTapSink() {
    QAudioFormat fmt;
    fmt.setSampleRate(int(m_tapRate));
    fmt.setChannelCount(int(m_tapChannels));
    fmt.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull() || !dev.isFormatSupported(fmt)) return false;

    if (!m_fifo) m_fifo = new AudioFifo();
    m_fifo->clear();

    delete m_sink;
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(fmt.bytesForDuration(100000)); // ~100ms: low latency, still glitch-safe
    m_sinkDev = m_sink->start(); // push mode: we write decoded PCM to this device
    if (!m_sinkDev || m_sink->error() != QAudio::NoError) {
        delete m_sink; m_sink = nullptr; m_sinkDev = nullptr;
        return false;
    }
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setInterval(8); // ~ keep the sink fed without busy-spinning
        connect(m_drainTimer, &QTimer::timeout, this, &PlaybackEngine::drainToSink);
    }
    m_drainTimer->start();
    return true;
}

void PlaybackEngine::stopTapSink() {
    if (m_drainTimer) m_drainTimer->stop();
    if (m_sink) { m_sink->stop(); delete m_sink; m_sink = nullptr; }
    m_sinkDev = nullptr;
    if (m_fifo) m_fifo->clear();
}

void PlaybackEngine::drainToSink() {
    if (!m_sink || !m_sinkDev || !m_fifo) return;
    const int free = m_sink->bytesFree();
    if (free <= 0) return;
    const QByteArray chunk = m_fifo->pop(free);
    if (chunk.isEmpty()) return;
    m_sinkDev->write(chunk);
    analyzeForVisualizer(chunk); // analyse at CONSUMPTION time (in sync with playback)
}

// Runs on this (GUI) thread from the drain timer, on the exact PCM being sent to
// the sink — so the visualizer tracks what's HEARD, not what VLC has decoded
// ahead (VLC front-loads the ring, which made the analysis run ~a buffer ahead).
// The only remaining lead is the sink's own buffer, held back by kVisualDelayMs.
void PlaybackEngine::analyzeForVisualizer(const QByteArray& chunk) {
    const int ch = qMax(1, int(m_tapChannels));
    const qint64 frames = (chunk.size() / 2) / ch;
    if (frames <= 0) return;
    const int16_t* s = reinterpret_cast<const int16_t*>(chunk.constData());

    // Bass/mid crossover ~330Hz, CASCADED to 12dB/oct so kick energy lands in the
    // bass band instead of bleeding up into mids. Mid/treble split ~2kHz (1-pole).
    const double aLow = 0.046, aMid = 0.250;
    double sumsq = 0.0, eBass = 0.0, eMid = 0.0, eTreb = 0.0;
    for (qint64 f = 0; f < frames; ++f) {
        const double x = (ch >= 2) ? (s[f * ch] + s[f * ch + 1]) * (0.5 / 32768.0)
                                   : s[f * ch] / 32768.0;
        sumsq += x * x;
        m_lpLow  += aLow * (x - m_lpLow);
        m_lpLow2 += aLow * (m_lpLow - m_lpLow2);  // second pole -> steeper bass edge
        m_lpMid  += aMid * (x - m_lpMid);
        const double bass = m_lpLow2;             // well-defined low band
        const double mid  = m_lpMid - m_lpLow2;   // bass removed more cleanly
        const double treb = x - m_lpMid;
        eBass += bass * bass; eMid += mid * mid; eTreb += treb * treb;
    }
    const double inv = 1.0 / double(frames);

    // Auto-gain: normalise each band to its OWN recent peak (fast attack, slow
    // decay) so it maps the track's dynamics into 0..1 and never just pins at full
    // on hot/bass-heavy material — there's always headroom and movement left.
    auto agc = [](double raw, double& peak) -> float {
        constexpr double decay  = 0.9990; // peak falls slowly (~seconds) -> stable auto-range
        constexpr double floorv = 0.06;   // higher gate so quiet bands don't chatter
        peak = std::max(raw, peak * decay);
        return float(qBound(0.0, raw / std::max(peak, floorv), 1.0));
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
    QTimer::singleShot(kVisualDelayMs, this, [this, level, bass, mid, treble, isBeat]() {
        emit audioLevel(level);
        emit audioSpectrum(bass, mid, treble);
        if (isBeat) emit beat();
    });
}

void PlaybackEngine::onAudioData(const void* samples, unsigned count, int64_t /*pts*/) {
    // VLC's audio thread: just hand the PCM to the ring. Analysis happens at
    // CONSUMPTION time in analyzeForVisualizer() (called from drainToSink) so the
    // visualizer stays in sync with what's actually being heard.
    if (!m_fifo) return;
    const qint64 nSamples = qint64(count) * m_tapChannels;
    m_fifo->push(static_cast<const char*>(samples), nSamples * 2); // S16 = 2 bytes
}
