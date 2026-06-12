#include "MainWindow.h"
#include "VideoPlayer.h"
#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QIcon>
#include <QLineEdit>
#include <QTextEdit>
#include <QScrollArea>
#include <QTabBar>
#include <QToolButton>
#include <QMenu>
#include <QLabel>
#include <QMovie>
#include <QFrame>
#include <QSettings>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QTimer>
#include <QCursor>
#include <QWindow>
#include <QProxyStyle>
#include <windows.h>
#include <winuser.h>

namespace {
// A tab's close button sits PM_TabBarTabHSpace/2 (=12px under Fusion) in from
// the tab's edge — dead space after our small round x. The label inset AND the
// tab's size hint derive from the same metric, so halving it tightens the
// trailing gap to 6px, narrows the tab to match, and the label keeps an exact
// fit (shrinking the tab any other way elides the label — the text's right
// boundary is measured from the tab edge, not the button).
class TabBarStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle; // no base style: tracks the app style (Fusion)
    int pixelMetric(PixelMetric metric, const QStyleOption* opt, const QWidget* w) const override {
        if (metric == PM_TabBarTabHSpace) return 12;
        return QProxyStyle::pixelMetric(metric, opt, w);
    }
};

// How far the cursor may leave the tab bar mid-drag before the tab tears off
// into its own window. Generous horizontally (reorder overshoot is normal),
// tight vertically (pulling a tab up/down reads as "take it out").
constexpr int kTearOffSlackX = 48;
constexpr int kTearOffSlackY = 26;
}

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
    // Close buttons are our own small round ones (see makeTabCloseButton), not
    // the style's setTabsClosable() squares.
    {   // setStyle doesn't take ownership — parent it to the bar
        auto* st = new TabBarStyle;
        st->setParent(tabs->tabBar());
        tabs->tabBar()->setStyle(st);
    }
    // Mouse watch for the tear-off gesture (drag a tab off the bar to float it).
    tabs->tabBar()->installEventFilter(this);

    // Floating "+" that trails the last tab: a menu of the closed tabs, click
    // one to reopen. Child of the tab widget, not the tab bar (the bar is only
    // as wide as its tabs and would clip it) — positionPlusButton() keeps it
    // snug to the last tab's right edge.
    m_plusBtn = new QToolButton(tabs);
    m_plusBtn->setObjectName("tabPlusButton"); // round look comes from the theme sheet
    m_plusBtn->setText("+");
    m_plusBtn->setFixedSize(18, 18);
    m_plusBtn->setCursor(Qt::PointingHandCursor);
    m_plusBtn->setToolTip("Open a closed tab");
    m_plusBtn->setPopupMode(QToolButton::InstantPopup);
    m_plusBtn->hide(); // positionPlusButton() shows it once it's actually placed
    m_plusMenu = new QMenu(m_plusBtn);
    m_plusBtn->setMenu(m_plusMenu);
    connect(m_plusMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildPlusMenu);
    // Drag-reordering moves the last tab's edge without any add/remove of ours.
    connect(tabs->tabBar(), &QTabBar::tabMoved, this, [this](int, int) { schedulePlusReposition(); });
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
    // Paint an opaque, themed page background. Without this the page is transparent,
    // so a splitter/window resize that exposes fresh page area smears the old pixels
    // (the bars/buttons "trail" like Win95). Window role keeps it the themed colour.
    scroll->viewport()->setAutoFillBackground(true);
    scroll->viewport()->setBackgroundRole(QPalette::Window);
    scroll->setWidget(tab);
    m_tabPages.insert(tab, scroll); // remember the wrapper so we can find the tab later
    m_tabOrder.append({ tab, scroll, label });
    installFilterRecursive(scroll, this);

    // Honour the remembered closed set (default empty: one of each tab open).
    // The page still lives — parented to the tab widget, just never inserted —
    // so its module keeps running and "+" can open it later.
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    if (cfg.value("Tabs/Closed").toStringList().contains(label)) {
        scroll->hide();
        scroll->setParent(tabs);
        return;
    }
    const int idx = tabs->addTab(scroll, label);
    tabs->tabBar()->setTabButton(idx, QTabBar::RightSide, makeTabCloseButton(scroll));
    schedulePlusReposition();
}

QWidget* MainWindow::makeTabCloseButton(QWidget* wrapper) {
    // QTabBar deletes tab buttons with their tab, so each (re)open makes a fresh
    // one. Identified by the wrapper, not an index — indexes shift under drags.
    auto* b = new QToolButton(tabs->tabBar());
    b->setObjectName("tabCloseButton"); // round look comes from the theme sheet
    b->setText(QString(QChar(0x00D7))); // multiplication x — cleaner than "x"
    b->setFixedSize(14, 14);
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip("Close tab");
    connect(b, &QToolButton::clicked, this, [this, wrapper]() {
        const int idx = tabs->indexOf(wrapper);
        if (idx >= 0) closeTabAt(idx);
        });
    return b;
}

void MainWindow::positionPlusButton() {
    QTabBar* bar = tabs->tabBar();
    if (bar->count() == 0) { m_plusBtn->hide(); return; }
    const QRect last = bar->tabRect(bar->count() - 1);
    // Tab-bar coords -> tab-widget coords; clamp so a crowded bar can't push
    // the button out of view.
    int x = bar->pos().x() + last.right() + 6;
    x = qMin(x, tabs->width() - m_plusBtn->width() - 4);
    const int y = bar->pos().y() + last.top() + (last.height() - m_plusBtn->height()) / 2;
    m_plusBtn->move(x, y);
    m_plusBtn->raise();
    m_plusBtn->show();
}

void MainWindow::schedulePlusReposition() {
    // Tab geometry is stale until the pending layout pass runs; measure after it.
    QTimer::singleShot(0, this, [this]() { positionPlusButton(); });
}

// --- Tear-off tabs -----------------------------------------------------------

FloatingTab::FloatingTab(QWidget* wrapper, const QString& label, MainWindow* host)
    : QWidget(nullptr, Qt::Window), m_wrapper(wrapper), m_label(label), m_host(host) {
    setWindowTitle(label + " - Seagull");
    setWindowIcon(host->windowIcon());
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(wrapper);
    wrapper->show();
}

void FloatingTab::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    m_host->floatingMoved(this);
}

void FloatingTab::closeEvent(QCloseEvent* event) {
    m_host->dockFloating(this, false); // closing never loses the tab — it docks
    event->accept();
}

void MainWindow::detachTab(QWidget* wrapper, const QPoint& globalPos) {
    const int idx = tabs->indexOf(wrapper);
    if (idx < 0 || tabs->count() <= 1) return; // the last docked tab stays put
    const QString label = tabs->tabText(idx);
    if (m_busyTab && m_tabPages.value(m_busyTab) == wrapper) m_busyTab = nullptr;
    tabs->removeTab(idx); // also deletes its close button / spinner

    auto* fl = new FloatingTab(wrapper, label, this);
    m_floating.append(fl);
    fl->resize(qMax(560, wrapper->width()), qMax(380, wrapper->height()));
    fl->move(globalPos - QPoint(120, 12)); // cursor lands on the new title bar
    fl->show();
    saveOpenTabs();
    schedulePlusReposition();

    // The mouse button is still down from the tear gesture; hand the drag to the
    // OS as a window move so the tab seamlessly "becomes" the window in hand.
    if (fl->windowHandle()) fl->windowHandle()->startSystemMove();
}

void MainWindow::floatingMoved(FloatingTab* fl) {
    // Dock zone: the tab-strip band across the full tab-widget width.
    QTabBar* bar = tabs->tabBar();
    const QRect strip(tabs->mapToGlobal(QPoint(0, bar->pos().y())),
                      QSize(tabs->width(), bar->height() + 8));
    const bool over = strip.contains(QCursor::pos());
    // The cursor starts inside the strip the moment the tab tears off — require
    // it to leave once before hovering the strip means "put it back".
    if (!fl->m_armed) { if (!over) fl->m_armed = true; return; }
    if (over && !isMinimized()) dockFloating(fl, true);
}

void MainWindow::dockFloating(FloatingTab* fl, bool atCursor) {
    if (!m_floating.removeOne(fl)) return; // re-entry guard (already docking)
    QWidget* wrapper = fl->m_wrapper;
    QTabBar* bar = tabs->tabBar();

    int at = atCursor ? bar->tabAt(bar->mapFromGlobal(QCursor::pos())) : -1;
    if (at < 0) { // no drop target — canonical spot, like reopenTab
        at = 0;
        for (const TabInfo& t : m_tabOrder) {
            if (t.wrapper == wrapper) break;
            const int i = tabs->indexOf(t.wrapper);
            if (i >= 0) at = qMax(at, i + 1);
        }
    }

    fl->hide();
    wrapper->setParent(tabs); // pull it out of the float before that dies
    const int idx = tabs->insertTab(at, wrapper, fl->m_label);
    bar->setTabButton(idx, QTabBar::RightSide, makeTabCloseButton(wrapper));
    tabs->setCurrentIndex(idx);
    fl->deleteLater();
    saveOpenTabs();
    schedulePlusReposition();
    activateWindow(); // focus follows the tab back to the shell
}

void MainWindow::closeTabAt(int index) {
    if (tabs->count() <= 1) return; // the last tab stays — never an empty tab bar

    QWidget* wrapper = tabs->widget(index);
    // Removing the tab also deletes its header buttons (incl. a busy spinner).
    if (m_busyTab && m_tabPages.value(m_busyTab) == wrapper) m_busyTab = nullptr;

    tabs->removeTab(index); // doesn't delete the page...
    wrapper->hide();
    wrapper->setParent(tabs); // ...keep it owned + alive while closed
    saveOpenTabs();
    schedulePlusReposition();
}

void MainWindow::reopenTab(int orderIdx) {
    const TabInfo& t = m_tabOrder[orderIdx];
    if (tabs->indexOf(t.wrapper) >= 0) return; // already open

    // Back to its canonical spot: right after the last currently-open tab that
    // was registered before it (works even if the user has dragged tabs around).
    int insertAt = 0;
    for (int i = 0; i < orderIdx; ++i) {
        const int idx = tabs->indexOf(m_tabOrder[i].wrapper);
        if (idx >= 0) insertAt = qMax(insertAt, idx + 1);
    }
    tabs->insertTab(insertAt, t.wrapper, t.label);
    tabs->tabBar()->setTabButton(insertAt, QTabBar::RightSide, makeTabCloseButton(t.wrapper));
    tabs->setCurrentIndex(insertAt); // they asked for it — show it
    saveOpenTabs();
    schedulePlusReposition();
}

void MainWindow::rebuildPlusMenu() {
    m_plusMenu->clear();
    for (int i = 0; i < m_tabOrder.size(); ++i) {
        if (tabs->indexOf(m_tabOrder[i].wrapper) >= 0) continue;       // open already
        if (m_tabOrder[i].wrapper->window() != this) continue;         // floating — open elsewhere
        m_plusMenu->addAction(m_tabOrder[i].label, this, [this, i]() { reopenTab(i); });
    }
    if (m_plusMenu->isEmpty())
        m_plusMenu->addAction("All tabs open")->setEnabled(false);
}

void MainWindow::saveOpenTabs() {
    // A tab is "closed" only when it's neither docked nor floating in its own
    // window (a closed page is parented back under the tab widget, so its
    // window() is this shell; a floating one's window() is the FloatingTab).
    QStringList closed;
    for (const TabInfo& t : m_tabOrder)
        if (tabs->indexOf(t.wrapper) < 0 && t.wrapper->window() == this) closed << t.label;
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    cfg.setValue("Tabs/Closed", closed);
}

void MainWindow::setTabBusy(QWidget* tab, bool busy) {
    QWidget* page = m_tabPages.value(tab, nullptr);
    if (!page) return;
    const int idx = tabs->indexOf(page);
    if (idx < 0) { // tab is closed — nothing to decorate (clear stale state though)
        if (m_busyTab == tab) m_busyTab = nullptr;
        return;
    }

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
        // LeftSide: the close button owns the right side now that tabs are closable.
        tabs->tabBar()->setTabButton(idx, QTabBar::LeftSide, spinner);
        m_busyTab = tab;
    }
    else {
        // Passing nullptr removes and deletes the spinner widget (and its movie).
        tabs->tabBar()->setTabButton(idx, QTabBar::LeftSide, nullptr);
        if (m_busyTab == tab) m_busyTab = nullptr;
    }
    schedulePlusReposition(); // the spinner widens/narrows the tab
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
    schedulePlusReposition();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (videoPlayer) videoPlayer->repositionOverlays();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // A hand-edited Tabs/Closed could list every tab; never come up with zero.
    if (tabs->count() == 0 && !m_tabOrder.isEmpty()) reopenTab(0);
    schedulePlusReposition();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Pull every floating tab back in: they get saved as open for next launch,
    // and no orphan window outlives the shell (which would keep the app alive).
    while (!m_floating.isEmpty()) dockFloating(m_floating.first(), false);
    QMainWindow::closeEvent(event);
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
    // --- Tear-off gesture on the tab bar ---
    if (watched == tabs->tabBar()) {
        auto* bar = tabs->tabBar();
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            // Remember the page, not the index — reorder drags shift indexes.
            const int at = bar->tabAt(me->position().toPoint());
            m_pressedWrapper = (me->button() == Qt::LeftButton && at >= 0) ? tabs->widget(at) : nullptr;
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            m_pressedWrapper = nullptr;
        }
        else if (event->type() == QEvent::MouseMove && m_pressedWrapper) {
            auto* me = static_cast<QMouseEvent*>(event);
            const QRect keep = bar->rect().adjusted(-kTearOffSlackX, -kTearOffSlackY,
                                                     kTearOffSlackX,  kTearOffSlackY);
            if (!keep.contains(me->position().toPoint())) {
                QWidget* wrapper = m_pressedWrapper;
                m_pressedWrapper = nullptr;
                // End the bar's internal reorder drag cleanly before tearing out.
                QMouseEvent release(QEvent::MouseButtonRelease, me->position(),
                    me->globalPosition(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(bar, &release);
                detachTab(wrapper, me->globalPosition().toPoint());
                return true;
            }
        }
        return false; // never swallow the bar's own handling otherwise
    }

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
