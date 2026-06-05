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

    // VLC Render Canvas
    videoWidget = new QFrame(videoContainer);
    videoWidget->setStyleSheet("background-color: black;");
    videoLayout->addWidget(videoWidget);

    // Initialize VLC
    vlcInstance = std::make_shared<VLC::Instance>(0, nullptr);
    vlcPlayer = std::make_shared<VLC::MediaPlayer>(*vlcInstance);

    // Defer handle assignment until the window is shown to avoid crashes
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

    connect(libraryModule, &Library::playMediaRequested, this, [this](const QUrl& url) {
        playVideo(url, QString());
        });

    connect(downloadsModule, &Downloads::playMediaRequested, this, [this](const QUrl& url, const QString& title) {
        playVideo(url, title);
        });

    mainSplitter->addWidget(tabs);
    layout->addWidget(mainSplitter);
    resize(1000, 700);
}

// Keep your existing helper functions below...
// (nativeEvent, installFilterRecursive, resizeEvent, moveEvent, etc.)

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
    playerControls->hide(); titleBar->hide();
    QMainWindow::resizeEvent(event);
    updateOverlayPosition();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    playerControls->hide(); titleBar->hide();
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
    default:
        QMainWindow::keyPressEvent(event);
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
    if (!videoWidget || !videoWidget->isVisible()) return;
    QPoint globalPos = videoWidget->mapToGlobal(QPoint(0, 0));
    titleBar->setFixedWidth(videoWidget->width());
    titleBar->move(globalPos.x(), globalPos.y() + 5);
    int controlX = globalPos.x() + (videoWidget->width() - playerControls->width()) / 2;
    int controlY = globalPos.y() + videoWidget->height() - playerControls->height() - 5;
    playerControls->move(controlX, controlY);
}

void MainWindow::showOSD() {
    if (!playerControls->isVisible()) {
        playerControls->show(); titleBar->show();
        updateOverlayPosition();
    }
    osdTimer->start(3000);
}

void MainWindow::hideOSD() {
    playerControls->hide(); titleBar->hide();
}

void MainWindow::playVideo(const QUrl& fileUrl, const QString& title) {
    videoContainer->show();
    mainSplitter->setSizes({ 600, 300 });

    if (!title.isEmpty()) {
        titleBar->setTitle(title);
    }
    else if (fileUrl.isLocalFile()) {
        QFileInfo fileInfo(fileUrl.toLocalFile());
        titleBar->setTitle(fileInfo.completeBaseName());
    }
    else {
        titleBar->setTitle("Streaming...");
    }

    playerControls->setStreamingMode(!fileUrl.isLocalFile());

    VLC::Media media(*vlcInstance, fileUrl.toString().toUtf8().constData(), VLC::Media::FromLocation);
    vlcPlayer->setMedia(media);
    vlcPlayer->play();
    showOSD();
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