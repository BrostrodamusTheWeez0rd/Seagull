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
#include <QComboBox>
#include <QAbstractSpinBox>
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
#include <QPainter>
#include <QProxyStyle>
#include <QPropertyAnimation>
#include <QStyle>
#include <windows.h>
#include <winuser.h>

namespace {
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
    mainSplitter->setHandleWidth(2);     // syncTabsPaneState hides it (0) only in fullscreen, pane down

    tabs = new QTabWidget(this);
    tabs->setMovable(true); // let the user drag tabs into any order
    // Close buttons are our own small round ones, placed manually (see
    // addCloseButton / positionCloseButtons), not the style's setTabsClosable()
    // squares and not the QTabBar RightSide slot — the QSS-styled tabs park slot
    // buttons at the far edge of the reserved padding, leaving a big gap.
    {   // setStyle doesn't take ownership — parent it to the bar. Pin the base to
        // Fusion explicitly: a base-less QProxyStyle latches onto whatever the app
        // style is at first paint, and the tab bar paints (in the Seagull ctor)
        // before Theme::apply switches to Fusion in run(). On Win11 the native
        // style is palette-aware so it still looked dark; on Win10 the native
        // style ignores the palette and the tab bar came out white.
        auto* st = new QProxyStyle("Fusion");
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

    // Floating Share, right of the "+": appears while an online video plays.
    m_shareBtn = new QToolButton(tabs);
    m_shareBtn->setObjectName("tabShareButton"); // round look comes from the theme sheet
    m_shareBtn->setFixedSize(18, 18);
    m_shareBtn->setIconSize(QSize(12, 12));
    m_shareBtn->setCursor(Qt::PointingHandCursor);
    m_shareBtn->setToolTip("Copy video link");
    m_shareBtn->hide();
    connect(m_shareBtn, &QToolButton::clicked, this, [this]() { emit shareRequested(); });
    // Drag-reordering moves the last tab's edge without any add/remove of ours.
    connect(tabs->tabBar(), &QTabBar::tabMoved, this, [this](int, int) { schedulePlusReposition(); });
    // Selecting a tab can shift tab widths; re-tuck the manual close buttons.
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) { schedulePlusReposition(); });
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
    syncTabsPaneState();
}

void MainWindow::syncTabsPaneState() {
    const bool open = mainSplitter->sizes().value(1) > 0;
    // Fullscreen with the pane down hides the handle completely (no grey seam
    // across the bottom of the screen); the hover chevron remains the way in.
    // With the pane up, and always in windowed mode, the 2px handle shows.
    mainSplitter->setHandleWidth(isFullScreen() && !open ? 0 : 2);
    if (videoPlayer) videoPlayer->setTabsPaneOpen(open);
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

void MainWindow::collapseTabs() {
    if (!videoPlayer || !videoPlayer->isVisible()) return; // no video to give the space to
    const QList<int> s = mainSplitter->sizes();
    if (s.value(1) <= 0) return; // already fully down
    // Remember where to return to on the next click. Fullscreen's immersive
    // sizes are never the split the user "left it at", so don't capture there.
    if (!isFullScreen()) captureSplit();
    mainSplitter->setSizes({ s.value(0) + s.value(1), 0 });
    syncTabsPaneState();
    videoPlayer->repositionOverlays();
}

void MainWindow::toggleTabsCollapsed() {
    if (!videoPlayer || !videoPlayer->isVisible()) return;
    if (mainSplitter->sizes().value(1) > 0) {
        collapseTabs();
    } else {
        applyStoredSplit();
        videoPlayer->repositionOverlays();
    }
}

QWidget* MainWindow::wrapPage(QWidget* tab) {
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
    installFilterRecursive(scroll, this);
    return scroll;
}

void MainWindow::addTab(QWidget* tab, const QString& label) {
    QWidget* scroll = wrapPage(tab);
    m_tabOrder.append({ tab, scroll, label });

    // Honour the remembered closed set (default empty: one of each tab open).
    // The page still lives — parented to the tab widget, just never inserted —
    // so its module keeps running and "+" can open it later.
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    if (cfg.value("Tabs/Closed").toStringList().contains(label)) {
        scroll->hide();
        scroll->setParent(tabs);
        return;
    }
    tabs->addTab(scroll, label);
    addCloseButton(scroll);
    schedulePlusReposition();
}

void MainWindow::openDynamicTab(QWidget* tab, const QString& label) {
    QWidget* wrapper = m_tabPages.value(tab, nullptr);
    if (!wrapper) wrapper = wrapPage(tab); // first appearance — wrap + register
    if (tabs->indexOf(wrapper) >= 0) return; // already open
    // Not appended to m_tabOrder: dynamic tabs aren't persisted and the "+"
    // menu never offers them — they exist only while their content does.
    tabs->addTab(wrapper, label); // at the end of the bar
    addCloseButton(wrapper);
    schedulePlusReposition();
}

void MainWindow::closeDynamicTab(QWidget* tab) {
    QWidget* wrapper = m_tabPages.value(tab, nullptr);
    if (!wrapper) return;
    const int idx = tabs->indexOf(wrapper);
    if (idx < 0) return;
    tabs->removeTab(idx);
    removeCloseButton(wrapper);
    wrapper->hide();
    wrapper->setParent(tabs); // keep the page alive for its next appearance
    schedulePlusReposition();
}

void MainWindow::setShareAvailable(bool on) {
    m_shareAvailable = on;
    if (on) {
        // Tint the glyph to the theme's text colour at show time (a stylesheet
        // can't recolour an icon; re-shown per video, so theme switches catch up).
        QPixmap pm = QIcon(":/Assets/icons/share.svg").pixmap(QSize(12, 12));
        if (!pm.isNull()) {
            QPainter p(&pm);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pm.rect(), palette().color(QPalette::Text));
            p.end();
            m_shareBtn->setIcon(QIcon(pm));
        }
    }
    schedulePlusReposition();
}

void MainWindow::addCloseButton(QWidget* wrapper) {
    if (m_closeButtons.contains(wrapper)) return; // already has one
    // A free child of the tab bar (not the RightSide slot): positionCloseButtons
    // tucks it tight to the tab's edge. Keyed by the wrapper, not an index —
    // indexes shift under drags. We own its lifetime now (removeCloseButton).
    auto* b = new QToolButton(tabs->tabBar());
    b->setObjectName("tabCloseButton"); // round themed chip from the theme sheet
    b->setText(QString(QChar(0x00D7))); // multiplication x — cleaner than "x"
    b->setFixedSize(14, 14);
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip("Close tab");
    connect(b, &QToolButton::clicked, this, [this, wrapper]() {
        const int idx = tabs->indexOf(wrapper);
        if (idx >= 0) closeTabAt(idx);
        });
    m_closeButtons.insert(wrapper, b);
    b->show();
}

void MainWindow::removeCloseButton(QWidget* wrapper) {
    if (QToolButton* b = m_closeButtons.take(wrapper)) b->deleteLater();
}

void MainWindow::positionCloseButtons() {
    // Manual placement: the QSS-styled tabs park a RightSide-slot button at the
    // far edge of the reserved right padding (a big gap after the label), so we
    // overlay our own x on each tab and tuck it against the tab's right edge.
    QTabBar* bar = tabs->tabBar();
    constexpr int inset = 6; // gap from the tab's right edge to the button
    const int curX = bar->mapFromGlobal(QCursor::pos()).x(); // for the grabbed tab
    const bool dragging = m_tabDragging && m_pressedWrapper;
    QWidget* busyWrapper = m_busyTab ? m_tabPages.value(m_busyTab) : nullptr;
    bool spinnerPlaced = false;
    QToolButton* draggedBtn = nullptr;
    for (auto it = m_closeButtons.cbegin(); it != m_closeButtons.cend(); ++it) {
        QToolButton* b = it.value();
        // A button sliding home after a drop owns its own position — don't yank it.
        if (it.key() == m_settlingWrapper) { b->show(); b->raise(); continue; }
        const bool isDragged = (it.key() == m_pressedWrapper);
        // Mid-drag, the dragged tab is the foreground element: hide every other
        // x so it can't bleed through onto it (the buttons are child widgets that
        // always paint over the bar's tab rendering). They return on drop.
        if (dragging && !isDragged) { b->hide(); continue; }
        const int idx = tabs->indexOf(it.key());
        if (idx < 0) { b->hide(); continue; } // tab not currently in the bar
        const QRect r = bar->tabRect(idx);
        // Hide buttons for tabs scrolled out of the visible strip (overflow).
        if (r.isEmpty() || r.right() <= 0 || r.left() >= bar->width()) { b->hide(); continue; }
        // A busy tab shows the spinner IN PLACE OF its x — centre it on the x's slot.
        if (it.key() == busyWrapper && m_tabSpinner) {
            b->hide();
            const int slotCx = r.right() - inset - b->width() / 2;
            m_tabSpinner->move(slotCx - m_tabSpinner->width() / 2,
                               r.top() + (r.height() - m_tabSpinner->height()) / 2);
            m_tabSpinner->raise();
            m_tabSpinner->show();
            spinnerPlaced = true;
            continue;
        }
        // The tab being dragged moves 1:1 with the cursor, so track its button to
        // the cursor (tabRect would lag at the settled slot); the rest snap to slot.
        const int x = isDragged
            ? qBound(0, curX + m_dragCloseDx, bar->width() - b->width())
            : r.right() - inset - b->width();
        b->move(x, r.top() + (r.height() - b->height()) / 2);
        b->show();
        if (isDragged) draggedBtn = b; else b->raise();
    }
    if (draggedBtn) draggedBtn->raise(); // keep the grabbed tab's x on top
    if (m_tabSpinner && !spinnerPlaced) m_tabSpinner->hide(); // busy tab gone/scrolled away
}

void MainWindow::settleCloseButton(QWidget* wrapper) {
    // On drop, QTabBar slides the tab from its lifted spot into its slot. Slide the
    // close x along the same way (matching the style's animation duration) instead
    // of snapping it straight to the slot ahead of the tab.
    QToolButton* b = m_closeButtons.value(wrapper);
    if (!b) return;
    QTabBar* bar = tabs->tabBar();
    const int idx = tabs->indexOf(wrapper);
    if (idx < 0) return;
    const QRect r = bar->tabRect(idx);
    constexpr int inset = 6;
    const QPoint target(r.right() - inset - b->width(), r.top() + (r.height() - b->height()) / 2);
    if (b->pos() == target) return;

    const int dur = bar->style()->styleHint(QStyle::SH_Widget_Animation_Duration);
    if (dur <= 0) { b->move(target); return; } // style animations off — snap, like the tab does

    auto* anim = new QPropertyAnimation(b, "pos", this);
    anim->setDuration(dur);                 // QTabBar reads the same hint, so they stay in sync
    anim->setStartValue(b->pos());
    anim->setEndValue(target);
    m_settlingWrapper = wrapper;
    m_settleAnim = anim;
    connect(anim, &QPropertyAnimation::finished, this, [this, wrapper]() {
        if (m_settlingWrapper == wrapper) m_settlingWrapper = nullptr;
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::positionPlusButton() {
    QTabBar* bar = tabs->tabBar();
    if (bar->count() == 0) { m_plusBtn->hide(); m_shareBtn->hide(); return; }
    const QRect last = bar->tabRect(bar->count() - 1);
    // Tab-bar coords -> tab-widget coords; clamp so a crowded bar can't push
    // the buttons out of view.
    const int reserved = m_plusBtn->width() + (m_shareAvailable ? m_shareBtn->width() + 4 : 0);
    int x = bar->pos().x() + last.right() + 6;
    x = qMin(x, tabs->width() - reserved - 4);
    const int y = bar->pos().y() + last.top() + (last.height() - m_plusBtn->height()) / 2;
    m_plusBtn->move(x, y);
    m_plusBtn->raise();
    m_plusBtn->show();

    if (m_shareAvailable) {
        m_shareBtn->move(x + m_plusBtn->width() + 4, y);
        m_shareBtn->raise();
        m_shareBtn->show();
    } else {
        m_shareBtn->hide();
    }
}

void MainWindow::schedulePlusReposition() {
    // Tab geometry is stale until the pending layout pass runs; measure after it.
    QTimer::singleShot(0, this, [this]() { positionCloseButtons(); positionPlusButton(); });
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

// --- Popped-out player window ------------------------------------------------

PlayerWindow::PlayerWindow(MainWindow* host)
    : QWidget(nullptr, Qt::Window), m_host(host) {
    setWindowTitle("Seagull");
    setWindowIcon(host->windowIcon());
    setMinimumSize(560, 315); // matches the player's own floor
}

void PlayerWindow::closeEvent(QCloseEvent* event) {
    // Closing the floating window is a hard stop: tear playback down. closePlayer()
    // emits closed(), whose handler re-docks the player into the shell and hides the
    // video area — so the player returns home, just with nothing playing.
    if (m_host->videoPlayer) m_host->videoPlayer->hardStop();
    event->accept();
}

void PlayerWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        if (isFullScreen()) {
            showNormal();
            QTimer::singleShot(100, this, [this]() {
                if (m_host->videoPlayer) {
                    m_host->videoPlayer->repositionOverlays();
                    m_host->videoPlayer->raiseOverlays();
                }
                });
        }
        break;
    case Qt::Key_Space:
        if (m_host->videoPlayer) m_host->videoPlayer->togglePlayPause();
        break;
    default: QWidget::keyPressEvent(event);
    }
}

// The overlays are top-level windows glued to the video frame's screen position,
// so the floating window must nudge them whenever it moves or resizes.
void PlayerWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    if (m_host->videoPlayer) m_host->videoPlayer->repositionOverlays();
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_host->videoPlayer) m_host->videoPlayer->repositionOverlays();
}

void MainWindow::detachTab(QWidget* wrapper, const QPoint& globalPos) {
    const int idx = tabs->indexOf(wrapper);
    if (idx < 0 || tabs->count() <= 1) return; // the last docked tab stays put
    const QString label = tabs->tabText(idx);
    if (m_busyTab && m_tabPages.value(m_busyTab) == wrapper) m_busyTab = nullptr;
    tabs->removeTab(idx);    // also deletes a LeftSide spinner (slot-managed)
    removeCloseButton(wrapper); // our x is a free child — delete it ourselves

    auto* fl = new FloatingTab(wrapper, label, this);
    m_floating.append(fl);
    fl->resize(qMax(560, wrapper->width()), qMax(380, wrapper->height()));
    fl->move(globalPos - QPoint(120, 12)); // rough spot to limit show() flash
    fl->show();

    // Now that the window is mapped its frame is known: drop it so the cursor sits
    // mid-title-bar. move() positions the frame top-left for a top-level window;
    // titleH (caption + top border) comes from frame vs client geometry, so this
    // holds across DPI/theme. startSystemMove then locks that cursor offset for the
    // whole OS drag, keeping the mouse on the title bar.
    const int titleH = fl->geometry().top() - fl->frameGeometry().top();
    const int wantY = titleH > 0 ? titleH / 2 : 15;            // mid-caption
    const int wantX = qMin(120, fl->frameGeometry().width() / 2);
    fl->move(globalPos.x() - wantX, globalPos.y() - wantY);

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
    addCloseButton(wrapper);
    tabs->setCurrentIndex(idx);
    fl->deleteLater();
    saveOpenTabs();
    schedulePlusReposition();
    activateWindow(); // focus follows the tab back to the shell
}

void MainWindow::closeTabAt(int index) {
    if (tabs->count() <= 1) return; // the last tab stays — never an empty tab bar

    QWidget* wrapper = tabs->widget(index);
    // Removing the tab deletes a slot-managed spinner; our close x is a free
    // child of the bar, so we drop it ourselves.
    if (m_busyTab && m_tabPages.value(m_busyTab) == wrapper) m_busyTab = nullptr;

    tabs->removeTab(index); // doesn't delete the page...
    removeCloseButton(wrapper);
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
    addCloseButton(t.wrapper);
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
        m_busyTab = tab;
        ensureTabSpinner();
        m_tabSpinnerMovie->start(); // positionCloseButtons drops it onto the busy tab's x
    }
    else {
        if (m_busyTab != tab) return;
        m_busyTab = nullptr;
        if (m_tabSpinnerMovie) m_tabSpinnerMovie->stop();
        if (m_tabSpinner) m_tabSpinner->hide();
    }
    schedulePlusReposition(); // re-place the x / spinner for this tab
}

void MainWindow::ensureTabSpinner() {
    if (m_tabSpinner) return;
    // One shared spinner (only one tab is ever busy at a time). It's a free child
    // of the tab bar, like the close buttons, and positionCloseButtons parks it on
    // the busy tab's x — replacing it rather than sitting beside the label.
    m_tabSpinner = new QLabel(tabs->tabBar());
    m_tabSpinner->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_tabSpinnerMovie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), m_tabSpinner);
    m_tabSpinnerMovie->jumpToFrame(0);
    const QSize f = m_tabSpinnerMovie->currentPixmap().size();
    const int h = 16; // small enough to sit inside the tab header
    const int w = f.height() > 0 ? f.width() * h / f.height() : h;
    m_tabSpinnerMovie->setScaledSize(QSize(w, h));
    m_tabSpinner->setMovie(m_tabSpinnerMovie);
    m_tabSpinner->resize(w, h);
    m_tabSpinner->hide();
}

void MainWindow::dockPlayerIntoSplitter() {
    // Put the player back on top of the tabs and (re-)wire the handle's click
    // filter — the handle is destroyed/recreated whenever the player leaves and
    // rejoins the splitter (pop-out / pop-in), so the filter must be reinstalled.
    mainSplitter->insertWidget(0, videoPlayer); // video on top, tabs below
    mainSplitter->setCollapsible(0, false);
    // Click-toggle on the splitter handle (collapse tabs / restore the split).
    // Registered on RELEASE in eventFilter so a click is never confused with a
    // drag; the handle exists now that both panes are in the splitter.
    if (QSplitterHandle* h = mainSplitter->handle(1))
        h->installEventFilter(this);
}

void MainWindow::setVideoPlayer(VideoPlayer* player) {
    videoPlayer = player;
    dockPlayerIntoSplitter();

    // The overlays are top-level windows in global coordinates, so anything that
    // moves the video surface (splitter drag, window move/resize) must nudge them.
    connect(mainSplitter, &QSplitter::splitterMoved, player, &VideoPlayer::repositionOverlays);
    // Keep the toggle chevron's arrow honest while the user drags the handle.
    connect(mainSplitter, &QSplitter::splitterMoved, this, [this](int, int) { syncTabsPaneState(); });

    // The chevron near the splitter toggles the pane like a handle click does.
    connect(player, &VideoPlayer::tabsToggleRequested, this, &MainWindow::toggleTabsCollapsed);
    syncTabsPaneState(); // initial arrow direction

    connect(player, &VideoPlayer::fullscreenToggleRequested, this, &MainWindow::toggleFullScreen);
    connect(player, &VideoPlayer::popOutRequested, this, &MainWindow::togglePlayerPopout);
    connect(player, &VideoPlayer::playbackStarted, this, [this]() {
        if (videoPlayer) videoPlayer->show();
        // Only apply the remembered split when the video area is collapsed (first
        // show); otherwise keep whatever size the user dragged it to this session.
        if (mainSplitter->sizes().value(0) <= 0) applyStoredSplit();
        });
    connect(player, &VideoPlayer::closed, this, [this]() {
        // If it was floating, bring it home first (re-docks + restores the split)
        // so we never capture a bogus ratio from the splitter's lone tabs pane.
        if (m_playerPopout) popInPlayer();
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
    mainSplitter->setSizes({ 1000, 0 }); // immersive: video fills, tabs collapsed
    syncTabsPaneState(); // hides the handle (pane is down); the chevron brings the tabs up
    QTimer::singleShot(100, this, [this]() {
        if (videoPlayer) { videoPlayer->repositionOverlays(); videoPlayer->raiseOverlays(); }
        });
}

void MainWindow::exitFullScreen() {
    if (m_wasMaximized) showMaximized(); else showNormal(); // restore the prior state
    applyStoredSplit(); // land back on the remembered split, not a fixed size
    QTimer::singleShot(100, this, [this]() {
        if (videoPlayer) { videoPlayer->repositionOverlays(); videoPlayer->raiseOverlays(); }
        });
}

void MainWindow::toggleFullScreen() {
    // While popped out, fullscreen acts on the floating window, not the shell.
    if (m_playerPopout) { togglePopoutFullScreen(); return; }
    if (isFullScreen()) exitFullScreen();
    else enterFullScreen();
}

void MainWindow::togglePopoutFullScreen() {
    if (!m_playerPopout) return;
    if (m_playerPopout->isFullScreen()) m_playerPopout->showNormal();
    else m_playerPopout->showFullScreen();
    QTimer::singleShot(100, this, [this]() {
        if (videoPlayer) { videoPlayer->repositionOverlays(); videoPlayer->raiseOverlays(); }
        });
}

// --- Player pop-out -----------------------------------------------------------

void MainWindow::togglePlayerPopout() {
    if (!videoPlayer || !videoPlayer->isVisible()) return; // only while something plays
    if (m_playerPopout) popInPlayer();
    else popOutPlayer();
}

void MainWindow::popOutPlayer() {
    if (!videoPlayer || m_playerPopout) return;
    if (!isFullScreen()) captureSplit(); // remember the docked split for pop-in

    auto* win = new PlayerWindow(this);
    auto* lay = new QVBoxLayout(win);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(videoPlayer); // single reparent out of the splitter (tabs fill the shell)
    m_playerPopout = win;

    videoPlayer->setPoppedOut(true);
    win->resize(960, 600);
    win->show();
    win->raise();
    win->activateWindow();
    videoPlayer->show();
    // The reparent recreated the render frame's HWND — hand VLC the new one
    // synchronously, before it draws another frame into the now-dead handle.
    videoPlayer->rebindOutputWindow();

    // Deferred: reownOverlays recreates the controls' window, and we're currently
    // inside that control's own click handler. Re-own once it returns, then
    // re-assert the pop-out's foreground (now that the overlays follow IT, raising
    // them keeps the pop-out forward instead of the shell).
    QTimer::singleShot(0, this, [this]() {
        if (!videoPlayer || !m_playerPopout) return;
        videoPlayer->reownOverlays();      // overlays -> pop-out owner
        m_playerPopout->raise();
        m_playerPopout->activateWindow();
        videoPlayer->repositionOverlays();
        videoPlayer->raiseOverlays();
        });
}

void MainWindow::popInPlayer() {
    if (!videoPlayer || !m_playerPopout) return;
    PlayerWindow* win = m_playerPopout;
    m_playerPopout = nullptr;
    if (win->isFullScreen()) win->showNormal(); // leave fullscreen before docking

    dockPlayerIntoSplitter();        // single reparent back on top of the tabs
    videoPlayer->setPoppedOut(false);
    videoPlayer->show();
    applyStoredSplit();              // restore the remembered video/tabs ratio
    videoPlayer->rebindOutputWindow(); // rebind synchronously (see popOutPlayer)

    // Deferred (we're inside the controls' click handler). CRITICAL ORDER: re-own
    // the overlays back to the shell BEFORE destroying the pop-out — the overlays
    // are owned by the pop-out's HWND right now, and destroying an owner also
    // destroys its owned windows (the controls), which is what was killing the
    // controls + mouse detection on pop-in. Foreground the shell so the overlay
    // exposure checks (and the OSD) start passing again.
    QTimer::singleShot(0, this, [this, win]() {
        if (videoPlayer) {
            videoPlayer->reownOverlays();   // overlays back under the shell's HWND...
            raise();
            activateWindow();
            videoPlayer->repositionOverlays();
            videoPlayer->raiseOverlays();
        }
        win->deleteLater();                 // ...THEN destroy the now-unrelated pop-out
        });
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
    // --- Click vs drag on the splitter handle ---
    // A clean click (press + release without ever crossing the drag threshold)
    // toggles the tabs pane: fully down / back to the remembered split. Real
    // drags keep working untouched: nothing is consumed, and any movement past
    // the threshold latches m_handleDragged so the release does nothing — even
    // a drag that ends back where it started. Registered on RELEASE only.
    if (watched == mainSplitter->handle(1)) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_handlePressed  = true;
                m_handleDragged  = false;
                m_handlePressPos = me->globalPosition().toPoint();
            }
        }
        else if (event->type() == QEvent::MouseMove && m_handlePressed) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->globalPosition().toPoint() - m_handlePressPos).manhattanLength()
                    >= QApplication::startDragDistance())
                m_handleDragged = true;
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            const bool clicked = m_handlePressed && !m_handleDragged
                              && me->button() == Qt::LeftButton;
            m_handlePressed = false;
            if (clicked) toggleTabsCollapsed();
        }
        return false; // never swallow — the splitter's own drag handling stays intact
    }

    // --- Tear-off gesture on the tab bar ---
    if (watched == tabs->tabBar()) {
        auto* bar = tabs->tabBar();
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            // Remember the page, not the index — reorder drags shift indexes.
            const int at = bar->tabAt(me->position().toPoint());
            m_pressedWrapper = (me->button() == Qt::LeftButton && at >= 0) ? tabs->widget(at) : nullptr;
            m_tabDragging = false; // a real drag latches on the first move below
            // Cancel a still-running slide-home so it can't fight a fresh grab.
            if (m_settleAnim) m_settleAnim->stop(); // DeleteWhenStopped clears the QPointer
            m_settlingWrapper = nullptr;
            // Lock the cursor-to-close-button offset so the grabbed tab's x can
            // follow the cursor 1:1 during the drag (tabRect only gives the
            // settled slot, which makes the button float then snap on release).
            if (m_pressedWrapper) {
                const QRect r = bar->tabRect(at);
                QToolButton* b = m_closeButtons.value(m_pressedWrapper);
                const int btnW = b ? b->width() : 14;
                m_dragCloseDx = (r.right() - 6 - btnW) - me->position().toPoint().x();
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            QWidget* dragged = m_pressedWrapper;
            m_pressedWrapper = nullptr;
            m_tabDragging = false;
            if (dragged) settleCloseButton(dragged); // slide its x home with the tab
            schedulePlusReposition(); // re-show the others; skips the settling one
        }
        else if (event->type() == QEvent::MouseMove && m_pressedWrapper) {
            auto* me = static_cast<QMouseEvent*>(event);
            m_tabDragging = true;   // first move latches a real drag (hides other x's)
            positionCloseButtons(); // track the close x's to the sliding tabs mid-drag
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
        // Don't steal typing in text inputs (except Escape, which leaves
        // fullscreen). Check the actual FOCUS widget, not just `watched`: the
        // press can arrive via any filtered widget while an editable combo's
        // line edit, a spin box, or a text field owns the keyboard.
        QWidget* fw = QApplication::focusWidget();
        const bool typing = qobject_cast<QLineEdit*>(watched) || qobject_cast<QTextEdit*>(watched)
                         || qobject_cast<QLineEdit*>(fw)      || qobject_cast<QTextEdit*>(fw)
                         || qobject_cast<QAbstractSpinBox*>(fw)
                         || qobject_cast<QComboBox*>(fw);
        if (typing && keyEvent->key() != Qt::Key_Escape) return false;
        keyPressEvent(keyEvent);
        if (keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Escape) return true;
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}
