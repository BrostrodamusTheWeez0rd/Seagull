#include "PlaybackEngine.h"
#include <QObject>
#include <QFile>
#include <QDir>

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
    m_player = std::make_shared<VLC::MediaPlayer>(*m_instance);
    m_player->setMouseInput(false);
    m_player->setKeyInput(false);

    hookEvents();
}

PlaybackEngine::~PlaybackEngine() {
    if (m_player) m_player->stop();
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

void PlaybackEngine::setVolume(int volume) { if (m_player) m_player->setVolume(volume); }
void PlaybackEngine::setMute(bool muted) { if (m_player) m_player->setMute(muted); }
