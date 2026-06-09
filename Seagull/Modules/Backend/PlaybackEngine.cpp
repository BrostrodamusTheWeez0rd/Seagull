#include "PlaybackEngine.h"
#include <QObject>

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

void PlaybackEngine::loadStream(const QUrl& videoUrl, const QUrl& audioUrl, qint64 startMs) {
    m_lastMedia = std::make_shared<VLC::Media>(*m_instance, videoUrl.toString().toUtf8().constData(), VLC::Media::FromLocation);

    // Use libavformat's demuxer (avoids VLC's native mp4 frag-sequence bugs),
    // pick the highest adaptive rendition, and auto-reconnect dropped HTTP
    // connections so the audio track doesn't go silent mid-stream.
    m_lastMedia->addOption(":demux=avformat");
    m_lastMedia->addOption(":network-caching=300");
    m_lastMedia->addOption(":adaptive-logic=highest");
    m_lastMedia->addOption(":http-reconnect=true");

    if (audioUrl.isValid() && !audioUrl.isEmpty())
        m_lastMedia->addOption(QString(":input-slave=" + audioUrl.toString()).toUtf8().constData());
    if (startMs > 0)
        m_lastMedia->addOption(QString(":start-time=%1").arg(startMs / 1000.0).toUtf8().constData());

    m_player->setMedia(*m_lastMedia);
}

void PlaybackEngine::reloadLastMedia() {
    if (!m_player || !m_lastMedia) return;
    m_player->stop();
    m_player->setMedia(*m_lastMedia);
}

bool PlaybackEngine::hasMedia() const { return m_lastMedia != nullptr; }

void PlaybackEngine::play() { if (m_player) m_player->play(); }
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
