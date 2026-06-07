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
        "--network-caching=5000",
        "--file-caching=5000"
    };

    vlcInstance = std::make_shared<VLC::Instance>(4, vlcArgs);
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
    if (playerControls) playerControls->show();
    if (titleBar) titleBar->show();
    updateOverlayPosition();
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

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    VLC::Media media(*vlcInstance, nativePath.toUtf8().constData(), VLC::Media::FromPath);
    vlcPlayer->setMedia(media);

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

void MainWindow::closePlayer() {
    if (!vlcPlayer) return;
    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();
    if (playerControls) playerControls->stopPolling();
    vlcPlayer->stop();
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
    updateOverlayPosition();
    osdTimer->start(3000);
}

void MainWindow::hideOSD() {
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
    VLC::Media media(*vlcInstance, videoUrl.toString().toUtf8().constData(), VLC::Media::FromLocation);
    if (audioUrl.isValid() && !audioUrl.isEmpty())
        media.addOption(QString(":input-slave=" + audioUrl.toString()).toUtf8().constData());
    if (savedStreamTimestamp > 0) {
        media.addOption(QString(":start-time=%1").arg(savedStreamTimestamp / 1000.0).toUtf8().constData());
        savedStreamTimestamp = -1;
    }
    vlcPlayer->setMedia(media);

    QTimer::singleShot(50, this, [this]() {
        vlcPlayer->play();
        if (playerControls) {
            playerControls->startPolling();
            playerControls->setCurrentFormat(lastRequestedFormatId); // Force sync here
        }
        });

    QPointer<PlayerControls> safeControls = playerControls;
    QTimer::singleShot(250, this, [safeControls]() {
        if (safeControls) safeControls->applyAudioState();
        });
}