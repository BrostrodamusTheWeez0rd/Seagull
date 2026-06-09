#include "MainWindow.h"
#include "VideoPlayer.h"
#include <QWidget>
#include <QVBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QIcon>
#include <QLineEdit>
#include <QTextEdit>
#include <QScrollArea>
#include <QTabBar>
#include <QLabel>
#include <QMovie>
#include <QFrame>
#include <QSettings>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QTimer>
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
    mainSplitter->setOpaqueResize(true); // live resize — follow the drag, don't wait for release

    tabs = new QTabWidget(this);
    tabs->setMovable(true); // let the user drag tabs into any order
    // Ignored vertical policy gives the tabs pane a zero minimum (it still expands
    // to fill when it's the only pane), so the splitter and the reveal-drag follow
    // the mouse continuously instead of snapping to the tab content's minimum.
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    mainSplitter->addWidget(tabs);

    layout->addWidget(mainSplitter);
    resize(1000, 700);

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_videoSplitRatio = qBound(0.1, settings.value("Display/VideoSplitRatio", 0.5).toDouble(), 0.9);
}

void MainWindow::applyStoredSplit() {
    // Express the ratio as two numbers; the splitter normalizes them to the actual
    // available height, so the proportion holds regardless of window size.
    const int v = qBound(1, int(m_videoSplitRatio * 1000), 999);
    mainSplitter->setSizes({ v, 1000 - v });
}

void MainWindow::captureSplit() {
    const QList<int> s = mainSplitter->sizes();
    const int total = s.value(0) + s.value(1);
    if (total <= 0) return;
    // Clamp so a fully-collapsed pane never becomes the saved default.
    m_videoSplitRatio = qBound(0.1, double(s.value(0)) / total, 0.9);
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    settings.setValue("Display/VideoSplitRatio", m_videoSplitRatio);
    settings.sync();
}

void MainWindow::addTab(QWidget* tab, const QString& label) {
    // Wrap each page in a scroll area so when the splitter squeezes the tab pane
    // (e.g. dragging the video area large), the page keeps its layout and scrolls
    // instead of its widgets overlapping each other.
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false); // keep the page's themed background
    scroll->setWidget(tab);
    tabs->addTab(scroll, label);
    m_tabPages.insert(tab, scroll); // remember the wrapper so we can find the tab later
    installFilterRecursive(scroll, this);
}

void MainWindow::setTabBusy(QWidget* tab, bool busy) {
    QWidget* page = m_tabPages.value(tab, nullptr);
    if (!page) return;
    const int idx = tabs->indexOf(page);
    if (idx < 0) return;

    if (busy) {
        if (m_busyTab == tab) return; // already spinning
        auto* spinner = new QLabel();
        auto* movie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), spinner);
        movie->jumpToFrame(0);
        const QSize f = movie->currentPixmap().size();
        const int h = 16; // small enough to sit inside the tab header
        const int w = f.height() > 0 ? f.width() * h / f.height() : h;
        movie->setScaledSize(QSize(w, h));
        spinner->setMovie(movie);
        movie->start();
        tabs->tabBar()->setTabButton(idx, QTabBar::RightSide, spinner);
        m_busyTab = tab;
    }
    else {
        // Passing nullptr removes and deletes the spinner widget (and its movie).
        tabs->tabBar()->setTabButton(idx, QTabBar::RightSide, nullptr);
        if (m_busyTab == tab) m_busyTab = nullptr;
    }
}

void MainWindow::setVideoPlayer(VideoPlayer* player) {
    videoPlayer = player;
    mainSplitter->insertWidget(0, player); // video on top, tabs below
    mainSplitter->setCollapsible(0, false);

    // The overlays are top-level windows in global coordinates, so anything that
    // moves the video surface (splitter drag, window move/resize) must nudge them.
    connect(mainSplitter, &QSplitter::splitterMoved, player, &VideoPlayer::repositionOverlays);

    connect(player, &VideoPlayer::fullscreenToggleRequested, this, &MainWindow::toggleFullScreen);
    connect(player, &VideoPlayer::playbackStarted, this, [this]() {
        if (videoPlayer) videoPlayer->show();
        // Only apply the remembered split when the video area is collapsed (first
        // show); otherwise keep whatever size the user dragged it to this session.
        if (mainSplitter->sizes().value(0) <= 0) applyStoredSplit();
        });
    connect(player, &VideoPlayer::closed, this, [this]() {
        // Remember the split the user left it at (but not the immersive fullscreen
        // sizes), to reuse as the default next time.
        if (!isFullScreen()) captureSplit();
        if (videoPlayer) videoPlayer->hide();
        if (isFullScreen()) exitFullScreen();
        });

    player->hide(); // nothing playing yet — let the tabs fill the window
}

void MainWindow::enterFullScreen() {
    m_wasMaximized = isMaximized(); // remember so we can restore it on exit
    showFullScreen();
    mainSplitter->setHandleWidth(0);     // hide the splitter; video fills the screen
    mainSplitter->setSizes({ 1000, 0 }); // immersive: video fills, tabs collapsed
    QTimer::singleShot(100, this, [this]() {
        if (videoPlayer) { videoPlayer->repositionOverlays(); videoPlayer->raiseOverlays(); }
        });
}

void MainWindow::exitFullScreen() {
    if (m_wasMaximized) showMaximized(); else showNormal(); // restore the prior state
    mainSplitter->setHandleWidth(2);
    applyStoredSplit(); // land back on the remembered split, not a fixed size
    QTimer::singleShot(100, this, [this]() {
        if (videoPlayer) { videoPlayer->repositionOverlays(); videoPlayer->raiseOverlays(); }
        });
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) exitFullScreen();
    else enterFullScreen();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_MOVING || msg->message == WM_MOVE) {
        if (videoPlayer) videoPlayer->repositionOverlays();
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::installFilterRecursive(QObject* obj, QObject* filter) {
    obj->installEventFilter(filter);
    for (QObject* child : obj->children()) installFilterRecursive(child, filter);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (videoPlayer) videoPlayer->repositionOverlays();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (videoPlayer) videoPlayer->repositionOverlays();
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        if (isFullScreen()) exitFullScreen();
        break;
    case Qt::Key_Space:
        if (videoPlayer) videoPlayer->togglePlayPause();
        break;
    default: QMainWindow::keyPressEvent(event);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        // Don't steal typing in text inputs (except Escape, which leaves fullscreen).
        if (qobject_cast<QLineEdit*>(watched) || qobject_cast<QTextEdit*>(watched)) {
            if (keyEvent->key() != Qt::Key_Escape) return false;
        }
        keyPressEvent(keyEvent);
        if (keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Escape) return true;
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}
