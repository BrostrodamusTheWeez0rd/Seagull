#include "VideoPlayer.h"
#include "../Backend/PlaybackEngine.h"
#include "../Backend/SgYtDlp.h"          // StreamOption
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"

#include <QVBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QTimer>
#include <QEvent>
#include <QResizeEvent>
#include <QCursor>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QRect>
#include <QSettings>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

VideoPlayer::VideoPlayer(QWidget* parent) : QWidget(parent) {
    // Floor for the video area so the splitter can't squeeze it away at small
    // window sizes (tunable). Width keeps the 500px controls pill from overhanging.
    setMinimumSize(560, 315);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    videoWidget = new QFrame(this);
    videoWidget->setStyleSheet("background-color: black;");
    layout->addWidget(videoWidget);

    engine = new PlaybackEngine(this);

    // winId() forces the native window to exist; defer so it's ready first.
    QTimer::singleShot(0, this, [this]() {
        engine->setOutputWindow((void*)videoWidget->winId());
        });

    connect(engine, &PlaybackEngine::endReached, this, &VideoPlayer::onMediaEndReached);
    connect(engine, &PlaybackEngine::paused, this, &VideoPlayer::showPosterOverlay);
    connect(engine, &PlaybackEngine::playing, this, [this]() {
        hidePosterOverlay();
        if (titleBar) titleBar->setLoading(false); // playback started — stop the seagull
        });
    connect(engine, &PlaybackEngine::errorOccurred, this, &VideoPlayer::onPlaybackError);

    updateOverlayTimer = new QTimer(this);
    updateOverlayTimer->setSingleShot(true);
    connect(updateOverlayTimer, &QTimer::timeout, this, &VideoPlayer::repositionOverlays);

    Qt::WindowFlags overlayFlags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;

    playerControls = new PlayerControls(engine, this);
    playerControls->setWindowFlags(overlayFlags);
    playerControls->setAttribute(Qt::WA_TranslucentBackground);

    titleBar = new PlayerTitleBar(this);
    titleBar->setWindowFlags(overlayFlags);
    titleBar->setAttribute(Qt::WA_TranslucentBackground);

    // Poster overlay: opaque image window that covers the video frame. Mouse
    // events pass straight through so clicking the video still toggles playback.
    posterOverlay = new QLabel(this);
    posterOverlay->setWindowFlags(overlayFlags | Qt::WindowTransparentForInput);
    posterOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    posterOverlay->setStyleSheet("background-color: black;");
    posterOverlay->setAlignment(Qt::AlignCenter);
    posterOverlay->setScaledContents(false);
    posterOverlay->hide();

    m_thumbNam = new QNetworkAccessManager(this);

    osdTimer = new QTimer(this);
    osdTimer->setSingleShot(true);
    connect(osdTimer, &QTimer::timeout, this, &VideoPlayer::hideOSD);

    mouseTrackerTimer = new QTimer(this);
    connect(mouseTrackerTimer, &QTimer::timeout, this, &VideoPlayer::checkMouseMovement);
    mouseTrackerTimer->start(100);

    clickTimer = new QTimer(this);
    clickTimer->setSingleShot(true);
    connect(clickTimer, &QTimer::timeout, this, &VideoPlayer::onSingleClickTimeout);

    lastMousePos = QCursor::pos();
    videoWidget->installEventFilter(this);

    connect(playerControls, &PlayerControls::qualitySelected, this, &VideoPlayer::changeStreamQuality);
    connect(playerControls, &PlayerControls::stopRequested, this, &VideoPlayer::handleStopRequest);
    connect(playerControls, &PlayerControls::replayRequested, this, &VideoPlayer::handleReplay);
    connect(playerControls, &PlayerControls::skipRequested, this, [this](int delta) { emit skipRequested(delta); });
    connect(playerControls, &PlayerControls::fullscreenRequested, this, [this]() { emit fullscreenToggleRequested(); });
}

void VideoPlayer::onMediaEndReached() {
    osdTimer->stop();
    // Freeze the seeker/timestamp at the end so they don't snap back to 0 while
    // VLC drains the decoder. startPolling() clears this when the next item plays.
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); }
    if (titleBar) titleBar->show();

    // Cover the final (often black) frame with the poster, controls on top.
    showPosterOverlay();
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();

    repositionOverlays();

    // If a next item exists, the auto-advance replaces the poster/replay; on the
    // last item nothing advances, so the poster + replay button persist.
    emit mediaEnded();
}

void VideoPlayer::playLocalFile(const QUrl& url) {
    emit playbackStarted(); // host shows + sizes the video area

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);

    // New media — drop any old poster (local files have no thumbnail to fetch).
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    engine->loadLocalFile(nativePath);

    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) playerControls->startPolling();
        });

    if (titleBar) { titleBar->setTitle(QFileInfo(nativePath).completeBaseName()); titleBar->setLoading(false); }
    if (playerControls) playerControls->setStreamingMode(false);
    showOSD();
}

void VideoPlayer::playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
    emit probeQualitiesRequested(rawUrl.toString());

    // New media — clear the old poster; the probe will resolve a fresh thumbnail.
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    if (playerControls) {
        playerControls->resetUiState();
        playerControls->setCurrentFormat("");
    }

    emit playbackStarted();

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);

    savedStreamTimestamp = -1;
    if (titleBar) {
        if (!title.isEmpty()) titleBar->setTitle(title);
        else titleBar->setTitle((cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) ? "Starting stream..." : "Probing stream...");
        titleBar->setLoading(true); // seagull spins until playing
    }

    if (playerControls) playerControls->setStreamingMode(true);

    currentBaseUrl = rawUrl;
    currentVideoTitle = title;

    if (cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) {
        onStreamUrlReady(cdnVideoUrl, cdnAudioUrl);
    }
    showOSD();
}

void VideoPlayer::handleStopRequest() {
    closePlayer();
}

void VideoPlayer::onThumbnailResolved(const QString& thumbUrl) {
    if (thumbUrl.isEmpty()) return;
    QNetworkRequest req((QUrl(thumbUrl)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_thumbNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pm;
        if (!pm.loadFromData(reply->readAll())) return; // e.g. webp w/o plugin
        m_posterPixmap = pm;
        // If we're already sitting paused/ended, paint it in now.
        if (posterOverlay && posterOverlay->isVisible())
            repositionOverlays();
        });
}

void VideoPlayer::showPosterOverlay() {
    if (!posterOverlay || m_posterPixmap.isNull()) return;
    if (!isVisible()) return;
    posterOverlay->show();
    repositionOverlays();              // sizes + paints the scaled pixmap
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();
}

void VideoPlayer::hidePosterOverlay() {
    if (posterOverlay) posterOverlay->hide();
}

void VideoPlayer::onPlaybackError() {
    // VLC couldn't open/play the stream. Tell the user and offer a retry: drop
    // into ended mode so the play button becomes a replay button (re-uses the
    // last media), and keep the controls/title pinned so the message stays put.
    osdTimer->stop();
    if (titleBar) { titleBar->setTitle("Stream failed to load — press replay to retry."); titleBar->setLoading(false); titleBar->show(); titleBar->raise(); }
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); playerControls->raise(); }
    repositionOverlays();
}

void VideoPlayer::handleReplay() {
    if (!engine->hasMedia()) return;
    if (playerControls) playerControls->setEndedMode(false);
    hidePosterOverlay();
    engine->reloadLastMedia();
    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) { playerControls->resetUiState(); playerControls->startPolling(); }
        });
    showOSD();
}

void VideoPlayer::closePlayer() {
    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();
    if (playerControls) { playerControls->stopPolling(); playerControls->setEndedMode(false); }
    engine->stop();
    hidePosterOverlay();
    playerControls->hide();
    titleBar->hide();
    emit closed(); // host hides the video area + leaves fullscreen
}

void VideoPlayer::onSingleClickTimeout() {
    togglePlayPause();
}

void VideoPlayer::togglePlayPause() {
    if (engine->isPlaying()) engine->pause();
    else engine->play();
}

bool VideoPlayer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == videoWidget) {
        if (event->type() == QEvent::MouseButtonPress) clickTimer->start(250);
        else if (event->type() == QEvent::MouseButtonDblClick) {
            clickTimer->stop();
            emit fullscreenToggleRequested();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoPlayer::repositionOverlays() {
    if (!videoWidget || !videoWidget->isVisible()) return;
    QPoint globalPos = videoWidget->mapToGlobal(QPoint(0, 0));
    if (posterOverlay && posterOverlay->isVisible()) {
        posterOverlay->setGeometry(globalPos.x(), globalPos.y(), videoWidget->width(), videoWidget->height());
        if (!m_posterPixmap.isNull())
            posterOverlay->setPixmap(m_posterPixmap.scaled(videoWidget->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (titleBar) { titleBar->setFixedWidth(videoWidget->width()); titleBar->move(globalPos.x(), globalPos.y() + 5); }
    if (playerControls && playerControls->isVisible()) {
        int controlX = globalPos.x() + (videoWidget->width() - playerControls->width()) / 2;
        int controlY = globalPos.y() + videoWidget->height() - playerControls->height() - 5;
        playerControls->move(controlX, controlY);
    }
}

void VideoPlayer::raiseOverlays() {
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();
}

void VideoPlayer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionOverlays();
}

void VideoPlayer::showOSD() {
    if (playerControls) playerControls->show();
    if (titleBar) titleBar->show();
    // Only re-stack above the poster when it's actually showing (paused / EOF).
    // Raising on every mouse-move otherwise buries the volume/quality popups.
    if (posterOverlay && posterOverlay->isVisible()) {
        if (playerControls) playerControls->raise();
        if (titleBar) titleBar->raise();
    }
    repositionOverlays();
    osdTimer->start(3000);
}

void VideoPlayer::hideOSD() {
    // Keep the OSD up while a volume/quality popup is open, otherwise hiding the
    // controls also tears down the popup the user is interacting with.
    if (playerControls && playerControls->hasOpenPopup()) {
        osdTimer->start(3000); // re-arm so it hides once the popup closes
        return;
    }
    if (engine->isPlaying()) {
        if (playerControls) playerControls->hide();
        if (titleBar) titleBar->hide();
    }
}

void VideoPlayer::checkMouseMovement() {
    if (!isVisible()) return;
    QPoint currentPos = QCursor::pos();
    if (currentPos != lastMousePos) {
        lastMousePos = currentPos;
        QRect videoRect(videoWidget->mapToGlobal(QPoint(0, 0)), videoWidget->size());
        if (videoRect.contains(currentPos)) showOSD();
    }
}

void VideoPlayer::handleAvailableQualities(const QList<StreamOption>& options) {
    // If the user hasn't manually picked a quality this session, highlight the
    // option matching the default Stream Quality setting so the OSD reflects reality.
    if (lastRequestedFormatId.isEmpty()) {
        QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
        QString defaultLabel = settings.value("Streaming/Quality", "Best Available").toString();

        if (!defaultLabel.isEmpty() && defaultLabel != "Best Available") {
            QRegularExpression re("(\\d+)");
            int wantHeight = re.match(defaultLabel).captured(1).toInt();

            QString bestMatchId;
            int bestMatchHeight = -1;
            for (const StreamOption& opt : options) {
                QString h = re.match(opt.label).captured(1);
                if (h.isEmpty()) continue;          // skip "Auto"
                int oh = h.toInt();
                if (oh <= wantHeight && oh > bestMatchHeight) {
                    bestMatchHeight = oh;
                    bestMatchId = opt.formatId;
                }
            }
            if (!bestMatchId.isEmpty() && playerControls)
                playerControls->setCurrentFormat(bestMatchId);
        }
    }

    playerControls->setAvailableQualities(options);
}

void VideoPlayer::changeStreamQuality(const QString& formatId) {
    lastRequestedFormatId = formatId;
    savedStreamTimestamp = engine->time();
    if (engine->isPlaying()) engine->pause();

    if (playerControls) playerControls->setCurrentFormat(formatId);

    if (titleBar) { titleBar->setTitle("Buffering new quality..."); titleBar->setLoading(true); }
    QTimer::singleShot(200, this, [this, formatId]() { emit streamUrlRequested(currentBaseUrl.toString(), formatId); });
}

void VideoPlayer::onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    if (titleBar) titleBar->setTitle(currentVideoTitle.isEmpty() ? "Streaming..." : currentVideoTitle);

    const qint64 startMs = (savedStreamTimestamp > 0) ? savedStreamTimestamp : 0;
    engine->loadStream(videoUrl, audioUrl, startMs);
    savedStreamTimestamp = -1;

    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) {
            playerControls->startPolling();
            playerControls->setCurrentFormat(lastRequestedFormatId); // make sure the menu shows the right one
        }
        });

    QPointer<PlayerControls> safeControls = playerControls;
    QTimer::singleShot(250, this, [safeControls]() {
        if (safeControls) safeControls->applyAudioState();
        });
}
