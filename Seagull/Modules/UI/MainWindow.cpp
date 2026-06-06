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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), isClosing(false) {
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

    QTimer::singleShot(0, this, [this]() {
        vlcPlayer->setHwnd((void*)videoWidget->winId());
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

    streamBackend = new SgYtDlp(this);

    connect(streamBackend, &SgYtDlp::availableQualitiesFound, this, &MainWindow::handleAvailableQualities);
    connect(streamBackend, &SgYtDlp::streamUrlReady, this, &MainWindow::onStreamUrlReady);
    connect(playerControls, &PlayerControls::qualitySelected, this, &MainWindow::changeStreamQuality);
    connect(playerControls, &PlayerControls::stopRequested, this, &MainWindow::handleStopRequest);

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

    auto* libraryModule = new Library();
    auto* downloadsModule = new Downloads();

    tabs->addTab(libraryModule, "Library");
    tabs->addTab(downloadsModule, "Downloads");
    tabs->addTab(new Search(), "Search");
    tabs->addTab(new Settings(), "Settings");

    installFilterRecursive(tabs, this);

    // Routing Library (Local Files) and Downloads (Streams) separately
    connect(libraryModule, &Library::playMediaRequested, this, &MainWindow::playLocalFile);
    connect(downloadsModule, &Downloads::playMediaRequested, this,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

    mainSplitter->addWidget(tabs);
    layout->addWidget(mainSplitter);

    resize(1000, 700);
}

void MainWindow::playLocalFile(const QUrl& url) {
    if (!videoWidget) return;

    isClosing = false;

    // RESTART TIMER: Ensure mouse detection works on new playback
    mouseTrackerTimer->start(100);

    videoContainer->show();
    mainSplitter->setSizes({ 600, 300 });

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    QByteArray pathData = nativePath.toUtf8();

    VLC::Media media(*vlcInstance, pathData.constData(), VLC::Media::FromPath);
    vlcPlayer->setMedia(media);
    vlcPlayer->play();

    if (titleBar) titleBar->setTitle(QFileInfo(nativePath).completeBaseName());
    if (playerControls) playerControls->setStreamingMode(false);

    showOSD();
}

void MainWindow::playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
    if (!videoWidget) return;

    isClosing = false;

    // RESTART TIMER: Ensure mouse detection works on new playback
    mouseTrackerTimer->start(100);

    videoContainer->show();
    mainSplitter->setSizes({ 600, 300 });

    savedStreamTimestamp = -1;

    if (titleBar) {
        if (!title.isEmpty()) titleBar->setTitle(title);
        else titleBar->setTitle((cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) ? "Starting stream..." : "Probing stream...");
    }

    if (playerControls)
        playerControls->setStreamingMode(true);

    currentBaseUrl = rawUrl;
    currentVideoTitle = title;

    if (cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) {
        onStreamUrlReady(cdnVideoUrl, cdnAudioUrl);
        streamBackend->probeAvailableQualities(rawUrl.toString());
    }
    else {
        streamBackend->probeAvailableQualities(rawUrl.toString());
    }

    showOSD();
}

void MainWindow::handleStopRequest() {
    closePlayer();
}

void MainWindow::closePlayer() {
    if (!vlcPlayer) return;

    isClosing = true;

    // Kill background process to prevent orphaned signals
    if (streamBackend) streamBackend->cancel();

    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();

    if (playerControls)
        playerControls->stopPolling();

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
    if (isClosing) return false;

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_MOVING || msg->message == WM_MOVE)
        updateOverlayPosition();

    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::installFilterRecursive(QObject* obj, QObject* filter) {
    obj->installEventFilter(filter);
    for (QObject* child : obj->children())
        installFilterRecursive(child, filter);
}

void MainWindow::scheduleUpdateOverlay() {
    updateOverlayTimer->start(0);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    if (videoContainer->isVisible()) {
        playerControls->hide();
        titleBar->hide();
    }

    QMainWindow::resizeEvent(event);
    updateOverlayPosition();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    if (videoContainer->isVisible()) {
        playerControls->hide();
        titleBar->hide();
    }

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
        if (isFullScreen()) {
            showNormal();
            mainSplitter->setHandleWidth(2);
            mainSplitter->setSizes({ 600, 300 });
        }
        break;

    case Qt::Key_Space:
        if (vlcPlayer->isPlaying()) vlcPlayer->pause();
        else vlcPlayer->play();
        break;

    default:
        QMainWindow::keyPressEvent(event);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (isClosing) return false;

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
        if (event->type() == QEvent::MouseButtonPress) {
            clickTimer->start(250);
        }
        else if (event->type() == QEvent::MouseButtonDblClick) {
            clickTimer->stop();
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
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateOverlayPosition() {
    if (isClosing || !videoWidget || !videoWidget->isVisible()) return;

    QPoint globalPos = videoWidget->mapToGlobal(QPoint(0, 0));

    if (titleBar) {
        titleBar->setFixedWidth(videoWidget->width());
        titleBar->move(globalPos.x(), globalPos.y() + 5);
    }

    if (playerControls && playerControls->isVisible()) {
        int controlX = globalPos.x() + (videoWidget->width() - playerControls->width()) / 2;
        int controlY = globalPos.y() + videoWidget->height() - playerControls->height() - 5;
        playerControls->move(controlX, controlY);
    }
}

void MainWindow::showOSD() {
    if (!playerControls->isVisible()) {
        playerControls->show();
        titleBar->show();
        updateOverlayPosition();
    }
    osdTimer->start(3000);
}

void MainWindow::hideOSD() {
    playerControls->hide();
    titleBar->hide();
}

void MainWindow::checkMouseMovement() {
    if (isClosing || !videoContainer->isVisible()) return;
    if (!vlcPlayer || !vlcPlayer->isPlaying()) return;

    QPoint currentPos = QCursor::pos();
    if (currentPos != lastMousePos) {
        lastMousePos = currentPos;
        QRect videoRect(videoWidget->mapToGlobal(QPoint(0, 0)), videoWidget->size());
        if (videoRect.contains(currentPos)) showOSD();
    }
}

void MainWindow::handleAvailableQualities(const QList<StreamOption>& options) {
    // Safety guard
    if (isClosing) return;

    playerControls->setAvailableQualities(options);
    if (!vlcPlayer->isPlaying()) {
        QTimer::singleShot(0, this, [this]() {
            if (!isClosing) streamBackend->fetchMetadataAndStreamUrl(currentBaseUrl.toString());
            });
    }
}

void MainWindow::changeStreamQuality(const QString& formatId) {
    savedStreamTimestamp = vlcPlayer->time();
    if (vlcPlayer->isPlaying()) vlcPlayer->pause();
    titleBar->setTitle("Buffering new quality...");
    QTimer::singleShot(200, this, [this, formatId]() {
        if (!isClosing) streamBackend->fetchMetadataAndStreamUrl(currentBaseUrl.toString(), formatId);
        });
}

void MainWindow::onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    // Safety guard
    if (isClosing) return;

    if (titleBar) titleBar->setTitle(currentVideoTitle.isEmpty() ? "Streaming..." : currentVideoTitle);

    VLC::Media media(*vlcInstance, videoUrl.toString().toUtf8().constData(), VLC::Media::FromLocation);

    if (audioUrl.isValid() && !audioUrl.isEmpty())
        media.addOption(QString(":input-slave=" + audioUrl.toString()).toUtf8().constData());

    if (savedStreamTimestamp > 0) {
        media.addOption(QString(":start-time=%1").arg(savedStreamTimestamp / 1000.0).toUtf8().constData());
        savedStreamTimestamp = -1;
    }

    vlcPlayer->setMedia(media);
    vlcPlayer->play();

    QPointer<PlayerControls> safeControls = playerControls;
    QTimer::singleShot(250, this, [safeControls]() {
        if (safeControls) safeControls->applyAudioState();
        });
}