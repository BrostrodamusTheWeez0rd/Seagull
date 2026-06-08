#include "MainWindow.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QCursor>
#include <QTimer>
#include <QSplitter>
#include <QIcon>
#include <QLineEdit>
#include <QTextEdit>
#include <windows.h>
#include <winuser.h>
#include <QPointer>
#include <QDir>
#include <QSettings>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QNetworkRequest>
#include <QNetworkReply>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowIcon(QIcon(":/Assets/Icon.ico"));
    setWindowTitle("Seagull");

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    mainSplitter = new QSplitter(Qt::Vertical, this);
    mainSplitter->setOpaqueResize(true);

    connect(mainSplitter, &QSplitter::splitterMoved, this, &MainWindow::updateOverlayPosition);

    videoContainer = new QWidget(this);
    videoContainer->setMinimumSize(600, 300);

    auto* videoLayout = new QVBoxLayout(videoContainer);
    videoLayout->setContentsMargins(0, 0, 0, 0);

    videoWidget = new QFrame(videoContainer);
    videoWidget->setStyleSheet("background-color: black;");
    videoLayout->addWidget(videoWidget);

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

    vlcInstance = std::make_shared<VLC::Instance>(6, vlcArgs);
    vlcPlayer = std::make_shared<VLC::MediaPlayer>(*vlcInstance);

    vlcPlayer->setMouseInput(false);
    vlcPlayer->setKeyInput(false);

    QTimer::singleShot(0, this, [this]() {
        vlcPlayer->setHwnd((void*)videoWidget->winId());
        vlcPlayer->setMouseInput(false);
        vlcPlayer->setKeyInput(false);
        });

    vlcPlayer->eventManager().onEndReached([this]() {
        QMetaObject::invokeMethod(this, "onMediaEndReached", Qt::QueuedConnection);
        });

    // Discrete state transitions (safe — not the high-frequency time callbacks):
    // show the poster thumbnail when paused, hide it when playback resumes.
    vlcPlayer->eventManager().onPaused([this]() {
        QMetaObject::invokeMethod(this, [this]() { showPosterOverlay(); }, Qt::QueuedConnection);
        });
    vlcPlayer->eventManager().onPlaying([this]() {
        QMetaObject::invokeMethod(this, [this]() { hidePosterOverlay(); }, Qt::QueuedConnection);
        });
    vlcPlayer->eventManager().onEncounteredError([this]() {
        QMetaObject::invokeMethod(this, [this]() { onPlaybackError(); }, Qt::QueuedConnection);
        });

    mainSplitter->addWidget(videoContainer);
    mainSplitter->setCollapsible(0, false);

    updateOverlayTimer = new QTimer(this);
    updateOverlayTimer->setSingleShot(true);
    connect(updateOverlayTimer, &QTimer::timeout, this, &MainWindow::updateOverlayPosition);

    Qt::WindowFlags overlayFlags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;

    playerControls = new PlayerControls(vlcPlayer.get(), this);
    playerControls->setWindowFlags(overlayFlags);
    playerControls->setAttribute(Qt::WA_TranslucentBackground);

    titleBar = new PlayerTitleBar(this);
    titleBar->setWindowFlags(overlayFlags);
    titleBar->setAttribute(Qt::WA_TranslucentBackground);

    // Poster overlay: opaque image window that covers the video frame. Mouse
    // events pass straight through so clicking the video still toggles playback.
    posterOverlay = new QLabel(this);
    // WindowTransparentForInput makes the OS pass clicks through to the video
    // window behind it, so play/pause-on-click and double-click-fullscreen work.
    posterOverlay->setWindowFlags(overlayFlags | Qt::WindowTransparentForInput);
    posterOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    posterOverlay->setStyleSheet("background-color: black;");
    posterOverlay->setAlignment(Qt::AlignCenter);
    posterOverlay->setScaledContents(false);
    posterOverlay->hide();

    m_thumbNam = new QNetworkAccessManager(this);

    osdTimer = new QTimer(this);
    osdTimer->setSingleShot(true);
    connect(osdTimer, &QTimer::timeout, this, &MainWindow::hideOSD);

    mouseTrackerTimer = new QTimer(this);
    connect(mouseTrackerTimer, &QTimer::timeout, this, &MainWindow::checkMouseMovement);
    mouseTrackerTimer->start(100);

    clickTimer = new QTimer(this);
    clickTimer->setSingleShot(true);
    connect(clickTimer, &QTimer::timeout, this, &MainWindow::onSingleClickTimeout);

    lastMousePos = QCursor::pos();
    videoWidget->installEventFilter(this);

    connect(playerControls, &PlayerControls::qualitySelected, this, &MainWindow::changeStreamQuality);
    connect(playerControls, &PlayerControls::stopRequested, this, &MainWindow::handleStopRequest);
    connect(playerControls, &PlayerControls::replayRequested, this, &MainWindow::handleReplay);
    connect(playerControls, &PlayerControls::skipRequested, this, [this](int delta) { emit skipRequested(delta); });

    connect(playerControls, &PlayerControls::fullscreenRequested, this, [this]() {
        if (isFullScreen()) {
            showNormal();
            mainSplitter->setHandleWidth(2);
            mainSplitter->setSizes({ 600, 300 });
        }
        else {
            showFullScreen();
            mainSplitter->setHandleWidth(0);
            mainSplitter->setSizes({ 1000, 0 });
        }
        QTimer::singleShot(100, this, [this]() {
            updateOverlayPosition();
            playerControls->raise();
            titleBar->raise();
            });
        });

    videoContainer->hide();
    tabs = new QTabWidget(this);
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainSplitter->addWidget(tabs);
    layout->addWidget(mainSplitter);
    resize(1000, 700);
}

void MainWindow::addTab(QWidget* tab, const QString& label) {
    tabs->addTab(tab, label);
    installFilterRecursive(tab, this);
}

void MainWindow::onMediaEndReached() {
    osdTimer->stop();
    // Freeze the seeker/timestamp at the end so they don't snap back to 0 while
    // VLC drains the decoder. startPolling() clears this when the next item plays.
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); }
    if (titleBar) titleBar->show();

    // Cover the final (often black) frame with the poster, controls on top.
    showPosterOverlay();
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();

    updateOverlayPosition();

    // If a next item exists, the auto-advance below replaces the poster/replay;
    // on the last item nothing advances, so the poster + replay button persist.
    emit mediaEnded();
}

void MainWindow::playLocalFile(const QUrl& url) {
    if (!videoWidget) return;

    videoContainer->show();
    mainSplitter->setSizes({ 600, 300 });

    vlcPlayer->setHwnd((void*)videoWidget->winId());
    vlcPlayer->setMouseInput(false);
    vlcPlayer->setKeyInput(false);

    mouseTrackerTimer->start(100);

    // New media — drop any old poster (local files have no thumbnail to fetch).
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    m_lastMedia = std::make_shared<VLC::Media>(*vlcInstance, nativePath.toUtf8().constData(), VLC::Media::FromPath);
    vlcPlayer->setMedia(*m_lastMedia);

    QTimer::singleShot(50, this, [this]() {
        vlcPlayer->play();
        if (playerControls) playerControls->startPolling();
        });

    if (titleBar) titleBar->setTitle(QFileInfo(nativePath).completeBaseName());
    if (playerControls) playerControls->setStreamingMode(false);
    showOSD();
}

void MainWindow::playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
    if (!videoWidget) return;

    emit probeQualitiesRequested(rawUrl.toString());

    // New media — clear the old poster; the probe will resolve a fresh thumbnail.
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    if (playerControls) {
        playerControls->resetUiState();
        playerControls->setCurrentFormat("");
    }

    videoContainer->show();
    mainSplitter->setSizes({ 600, 300 });

    vlcPlayer->setHwnd((void*)videoWidget->winId());
    vlcPlayer->setMouseInput(false);
    vlcPlayer->setKeyInput(false);

    mouseTrackerTimer->start(100);

    savedStreamTimestamp = -1;
    if (titleBar) {
        if (!title.isEmpty()) titleBar->setTitle(title);
        else titleBar->setTitle((cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) ? "Starting stream..." : "Probing stream...");
    }

    if (playerControls) playerControls->setStreamingMode(true);

    currentBaseUrl = rawUrl;
    currentVideoTitle = title;

    if (cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) {
        onStreamUrlReady(cdnVideoUrl, cdnAudioUrl);
    }
    showOSD();
}

void MainWindow::handleStopRequest() {
    closePlayer();
}

void MainWindow::onThumbnailResolved(const QString& thumbUrl) {
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
            updateOverlayPosition();
        });
}

void MainWindow::showPosterOverlay() {
    if (!posterOverlay || m_posterPixmap.isNull()) return;
    if (!videoContainer->isVisible()) return;
    posterOverlay->show();
    updateOverlayPosition();          // sizes + paints the scaled pixmap
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();
}

void MainWindow::hidePosterOverlay() {
    if (posterOverlay) posterOverlay->hide();
}

void MainWindow::onPlaybackError() {
    // VLC couldn't open/play the stream. Tell the user and offer a retry: drop
    // into ended mode so the play button becomes a replay button (re-uses
    // m_lastMedia), and keep the controls/title pinned so the message stays put.
    osdTimer->stop();
    if (titleBar) { titleBar->setTitle("Stream failed to load — press replay to retry."); titleBar->show(); titleBar->raise(); }
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); playerControls->raise(); }
    updateOverlayPosition();
}

void MainWindow::handleReplay() {
    if (!vlcPlayer || !m_lastMedia) return;
    if (playerControls) playerControls->setEndedMode(false);
    hidePosterOverlay();
    vlcPlayer->stop();
    vlcPlayer->setMedia(*m_lastMedia);
    QTimer::singleShot(50, this, [this]() {
        vlcPlayer->play();
        if (playerControls) { playerControls->resetUiState(); playerControls->startPolling(); }
        });
    showOSD();
}

void MainWindow::closePlayer() {
    if (!vlcPlayer) return;
    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();
    if (playerControls) playerControls->stopPolling();
    if (playerControls) playerControls->setEndedMode(false);
    vlcPlayer->stop();
    hidePosterOverlay();
    videoContainer->hide();
    playerControls->hide();
    titleBar->hide();
    if (isFullScreen()) {
        showNormal();
        mainSplitter->setHandleWidth(2);
        mainSplitter->setSizes({ 600, 300 });
    }
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_MOVING || msg->message == WM_MOVE) updateOverlayPosition();
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::installFilterRecursive(QObject* obj, QObject* filter) {
    obj->installEventFilter(filter);
    for (QObject* child : obj->children()) installFilterRecursive(child, filter);
}

void MainWindow::scheduleUpdateOverlay() { updateOverlayTimer->start(0); }

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    updateOverlayPosition();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    updateOverlayPosition();
}

void MainWindow::onSingleClickTimeout() {
    if (vlcPlayer->isPlaying()) vlcPlayer->pause();
    else vlcPlayer->play();
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        if (isFullScreen()) { showNormal(); mainSplitter->setHandleWidth(2); mainSplitter->setSizes({ 600, 300 }); }
        break;
    case Qt::Key_Space:
        if (vlcPlayer->isPlaying()) vlcPlayer->pause();
        else vlcPlayer->play();
        break;
    default: QMainWindow::keyPressEvent(event);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (qobject_cast<QLineEdit*>(watched) || qobject_cast<QTextEdit*>(watched)) {
            if (keyEvent->key() != Qt::Key_Escape) return false;
        }
        keyPressEvent(keyEvent);
        if (keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Escape) return true;
        return false;
    }
    if (watched == videoWidget) {
        if (event->type() == QEvent::MouseButtonPress) clickTimer->start(250);
        else if (event->type() == QEvent::MouseButtonDblClick) {
            clickTimer->stop();
            if (isFullScreen()) { showNormal(); mainSplitter->setHandleWidth(2); mainSplitter->setSizes({ 600, 300 }); }
            else { showFullScreen(); mainSplitter->setHandleWidth(0); mainSplitter->setSizes({ 1000, 0 }); }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateOverlayPosition() {
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

void MainWindow::showOSD() {
    if (playerControls) playerControls->show();
    if (titleBar) titleBar->show();
    // Only re-stack above the poster when it's actually showing (paused / EOF).
    // Raising on every mouse-move otherwise buries the volume/quality popups.
    if (posterOverlay && posterOverlay->isVisible()) {
        if (playerControls) playerControls->raise();
        if (titleBar) titleBar->raise();
    }
    updateOverlayPosition();
    osdTimer->start(3000);
}

void MainWindow::hideOSD() {
    // Keep the OSD up while a volume/quality popup is open, otherwise hiding the
    // controls also tears down the popup the user is interacting with.
    if (playerControls && playerControls->hasOpenPopup()) {
        osdTimer->start(3000); // re-arm so it hides once the popup closes
        return;
    }
    if (vlcPlayer && vlcPlayer->isPlaying()) {
        if (playerControls) playerControls->hide();
        if (titleBar) titleBar->hide();
    }
}

void MainWindow::checkMouseMovement() {
    if (!videoContainer->isVisible()) return;
    QPoint currentPos = QCursor::pos();
    if (currentPos != lastMousePos) {
        lastMousePos = currentPos;
        QRect videoRect(videoWidget->mapToGlobal(QPoint(0, 0)), videoWidget->size());
        if (videoRect.contains(currentPos)) showOSD();
    }
}

void MainWindow::handleAvailableQualities(const QList<StreamOption>& options) {
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

void MainWindow::changeStreamQuality(const QString& formatId) {
    lastRequestedFormatId = formatId;
    savedStreamTimestamp = vlcPlayer->time();
    if (vlcPlayer->isPlaying()) vlcPlayer->pause();

    if (playerControls) playerControls->setCurrentFormat(formatId);

    titleBar->setTitle("Buffering new quality...");
    QTimer::singleShot(200, this, [this, formatId]() { emit streamUrlRequested(currentBaseUrl.toString(), formatId); });
}

void MainWindow::onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    if (titleBar) titleBar->setTitle(currentVideoTitle.isEmpty() ? "Streaming..." : currentVideoTitle);
    m_lastMedia = std::make_shared<VLC::Media>(*vlcInstance, videoUrl.toString().toUtf8().constData(), VLC::Media::FromLocation);

    // Use libavformat's demuxer (avoids VLC's native mp4 frag-sequence bugs),
    // pick the highest adaptive rendition, and auto-reconnect dropped HTTP
    // connections so the audio track doesn't go silent mid-stream.
    m_lastMedia->addOption(":demux=avformat");
    m_lastMedia->addOption(":network-caching=300");
    m_lastMedia->addOption(":adaptive-logic=highest");
    m_lastMedia->addOption(":http-reconnect=true");

    if (audioUrl.isValid() && !audioUrl.isEmpty())
        m_lastMedia->addOption(QString(":input-slave=" + audioUrl.toString()).toUtf8().constData());
    if (savedStreamTimestamp > 0) {
        m_lastMedia->addOption(QString(":start-time=%1").arg(savedStreamTimestamp / 1000.0).toUtf8().constData());
        savedStreamTimestamp = -1;
    }
    vlcPlayer->setMedia(*m_lastMedia);

    QTimer::singleShot(50, this, [this]() {
        vlcPlayer->play();
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