#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QPointer>

class QSplitter;
class QTabWidget;
class QToolButton;
class QSpinBox;
class QPropertyAnimation;
class QLabel;
class QMovie;
class QMenu;
class QKeyEvent;
class QResizeEvent;
class QMoveEvent;
class QShowEvent;
class QCloseEvent;
class VideoPlayer;
class MainWindow;

// A torn-off tab living as its own top-level window: created when a tab is
// dragged off the tab bar. Dragging it back over the main window's tab strip
// docks it again; closing it docks it too (a floating tab is never "lost").
class FloatingTab : public QWidget {
    Q_OBJECT
public:
    FloatingTab(QWidget* wrapper, const QString& label, MainWindow* host);

protected:
    void moveEvent(QMoveEvent* event) override;   // dragged over the strip -> redock
    void closeEvent(QCloseEvent* event) override; // closing redocks as well

private:
    friend class MainWindow;
    QWidget* m_wrapper;   // the tab's QScrollArea page, reparented in here
    QString  m_label;
    MainWindow* m_host;
    bool m_armed = false; // redock only after the cursor has left the strip once
};

// The player detached into its own top-level window (via the controls' pop-out
// button). Hosts the VideoPlayer while floating; closing it re-docks the player
// into the main window — playback never stops. MainWindow drives the move and
// keeps the top-level overlays glued as this window moves/resizes.
class PlayerWindow : public QWidget {
    Q_OBJECT
public:
    explicit PlayerWindow(MainWindow* host);

protected:
    void closeEvent(QCloseEvent* event) override;   // re-dock on close
    void keyPressEvent(QKeyEvent* event) override;   // space / escape while floating
    void moveEvent(QMoveEvent* event) override;      // keep overlays glued
    void resizeEvent(QResizeEvent* event) override;

private:
    MainWindow* m_host;
};

// Pure application shell: owns the window chrome, the tab area, and the splitter
// the video player and tabs live in. It holds no playback logic — it hosts the
// VideoPlayer and handles only the window-level concerns the player can't do for
// itself: fullscreen, and repositioning the top-level overlays on window move/resize.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void addTab(QWidget* tab, const QString& label);
    void setVideoPlayer(VideoPlayer* player); // host the player in the splitter

    // Show/hide an animated spinner on a tab's header (e.g. Library while a
    // download runs). Identified by the page widget so it survives tab reordering.
    void setTabBusy(QWidget* tab, bool busy);

    // Dynamic tabs: pages that come and go with app state (e.g. the playing
    // video's Description). They sit at the end of the bar, close like any tab,
    // but are never persisted to Tabs/Closed and never listed in the "+" menu.
    void openDynamicTab(QWidget* tab, const QString& label);
    void closeDynamicTab(QWidget* tab);

    // Floating Share button beside the "+", shown while an online video plays.
    void setShareAvailable(bool on);

    bool autoplayEnabled() const { return m_autoplayEnabled; }
    bool shuffleEnabled()  const { return m_shuffleEnabled; }
    int  photoIntervalSeconds() const { return m_photoIntervalSecs; }

    // Point the autoplay/shuffle toggles at a playback context so each content
    // type keeps its own remembered settings (e.g. YouTube autoplay is separate
    // from local-video autoplay). `photoMode` relabels autoplay -> "Slideshow"
    // and surfaces the interval spin box. Called by the orchestrator when media
    // starts. An empty key leaves the toggles as they are.
    void setPlaybackContext(const QString& contextKey, bool photoMode);

    // Splitter click-toggle: drop the tabs pane completely (video fills) or
    // return to the split it was at before the drop. Triggered by a clean
    // click on the splitter handle (see eventFilter) and by the orchestrator
    // when shorts viewing starts. Works in fullscreen too.
    void collapseTabs();
    void toggleTabsCollapsed();

    // Multiple-instance tabs. A "duplicable" kind (e.g. Search, File Explorer) gets
    // a "New <kind> tab" entry in the "+" menu; choosing it emits newTabRequested so
    // the orchestrator can build a fresh instance and hand it back via addDuplicateTab.
    // Unlike canonical tabs, a duplicate isn't persisted or reopenable, and closing it
    // disposes the page (the orchestrator deletes the instance via duplicateTabClosed).
    void registerDuplicableTab(const QString& kind, const QString& menuLabel);
    void addDuplicateTab(QWidget* page, const QString& label, bool switchTo = true);

signals:
    void shareRequested();                      // the floating Share button was clicked
    void autoplayChanged(bool enabled);         // the autoplay toggle was flipped
    void shuffleChanged(bool enabled);          // the shuffle toggle was flipped
    void photoIntervalChanged(int seconds);     // the slideshow interval spin changed
    void newTabRequested(const QString& kind);  // "+" menu -> open a new instance of `kind`
    void duplicateTabClosed(QWidget* page);     // a duplicate tab closed -> dispose its page

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    // Player transport keys: space play/pause, left/right seek ±5s, up/down volume,
    // comma/period frame-step, M mute, F + escape fullscreen. Routed player-first
    // whenever media is active and our app is focused (bows out only for text-entry
    // widgets, modal dialogs, and popups). Returns true if it consumed the key.
    // Called from the app-wide event filter.
    bool handleMediaKey(QKeyEvent* event);
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override; // docks all floating tabs first
    void changeEvent(QEvent* event) override;     // re-tint the share glyph on theme change

private:
    // Closable tabs: every page registered via addTab stays alive for the whole
    // session whether or not its tab is open (the modules keep working in the
    // background); closing just removes it from the tab widget. The "+" corner
    // button reopens closed tabs at their original (registration-order) spot.
    struct TabInfo {
        QWidget* page;     // the module widget
        QWidget* wrapper;  // its QScrollArea, what actually sits in the tab widget
        QString  label;
    };
    void closeTabAt(int index);     // from the tab's close button; keeps >= 1 open
    void reopenTab(int orderIdx);   // from the "+" menu; index into m_tabOrder
    void rebuildPlusMenu();         // fills the "+" menu with the closed tabs
    void saveOpenTabs();            // persist which tabs are closed (Tabs/Closed)
    void addCloseButton(QWidget* wrapper);    // create + track a tab's manual close x
    void removeCloseButton(QWidget* wrapper); // drop it when the tab leaves the bar
    void positionCloseButtons();              // tuck each x tight to its tab's edge
    void ensureTabSpinner();                  // lazily build the busy spinner (replaces the x)
    void settleCloseButton(QWidget* wrapper); // slide a dropped tab's x home to match the tab
    QWidget* wrapPage(QWidget* tab);               // QScrollArea shell every page gets
    void positionPlusButton();      // snug the "+" (and Share) against the last tab
    void schedulePlusReposition();  // ...after the pending layout pass settles
    void tintShareButton();         // recolour the share glyph to the theme's dim text
    void tintAutoplayButton();      // recolour the autoplay glyph (highlight when on, subtext when off)
    void tintShuffleButton();       // recolour the shuffle glyph (highlight when on, subtext when off)

    // Player pop-out: detach the VideoPlayer into its own window and back.
    friend class PlayerWindow;
    void dockPlayerIntoSplitter();  // (re)insert the player at splitter index 0 + wire the handle
    void togglePlayerPopout();      // pop-out button: float the player / re-dock it
    void popOutPlayer();
    void popInPlayer();
    void togglePopoutFullScreen();  // fullscreen acts on the floating window while popped

    // Tear-off: dragging a tab off the bar floats it in its own window.
    friend class FloatingTab;
    void detachTab(QWidget* wrapper, const QPoint& globalPos);
    void dockFloating(FloatingTab* fl, bool atCursor); // back into the bar
    void floatingMoved(FloatingTab* fl);               // redock check while dragged

    void installFilterRecursive(QObject* obj, QObject* filter);
    void enterFullScreen();
    void exitFullScreen();
    void toggleFullScreen();
    void applyStoredSplit(); // size the splitter to the remembered video/tabs ratio
    void captureSplit();     // remember the current ratio (persisted to config.ini)
    void syncTabsPaneState();// push open/collapsed to the player's toggle chevron

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QHash<QWidget*, QWidget*> m_tabPages; // inner page widget -> its QScrollArea wrapper
    QList<TabInfo> m_tabOrder;            // every registered (canonical) tab, in addTab order
    QHash<QWidget*, QWidget*> m_duplicateTabs;       // duplicate tab wrapper -> its page (disposed on close)
    QHash<QWidget*, QString> m_duplicateKindMap;    // duplicate tab wrapper -> kind label (for persistence)
    QList<QPair<QString, QString>> m_duplicableKinds; // (kind, "New <kind> tab" menu label)
    QHash<QWidget*, QToolButton*> m_closeButtons; // open tab wrapper -> its manual close x
    QLabel* m_tabSpinner = nullptr;       // busy spinner shown in place of the busy tab's x
    QMovie* m_tabSpinnerMovie = nullptr;  // the seagull animation driving m_tabSpinner
    QToolButton* m_plusBtn = nullptr;      // "+" corner button (reopen menu)
    QToolButton* m_autoplayBtn = nullptr;  // autoplay toggle, between "+" and Share
    bool         m_autoplayEnabled = true;
    bool         m_playbackActive = false; // only show the autoplay toggle while media plays
    QToolButton* m_shuffleBtn = nullptr;   // shuffle toggle, shown only while autoplay is on
    bool         m_shuffleEnabled = false;
    QSpinBox*    m_photoIntervalSpin = nullptr; // slideshow seconds, shown only in photo mode
    int          m_photoIntervalSecs = 5;
    bool         m_photoMode = false;      // current context is a photo (slideshow chrome)
    QString      m_contextKey;             // settings namespace for the active content type
    QToolButton* m_shareBtn = nullptr;     // floating Share, right of the autoplay button
    bool m_shareAvailable = false;
    QMenu* m_plusMenu = nullptr;
    QList<FloatingTab*> m_floating;       // torn-off tabs currently in own windows
    QWidget* m_pressedWrapper = nullptr;  // tab page under the mouse press (tear-off)
    int m_dragCloseDx = 0;                // cursor->close-button x offset for the grabbed tab
    bool m_tabDragging = false;           // a tab reorder drag is in progress
    QWidget* m_settlingWrapper = nullptr; // tab whose x is mid slide-home (skip in positioning)
    QPointer<QPropertyAnimation> m_settleAnim; // the running slide-home animation, if any
    QWidget* m_busyTab = nullptr;         // page currently showing the spinner, if any
    VideoPlayer* videoPlayer = nullptr; // hosted; owned by the widget tree
    PlayerWindow* m_playerPopout = nullptr; // the floating player window, when popped out
    bool m_wasMaximized = false;        // window state before going fullscreen
    double m_videoSplitRatio = 0.5;     // video fraction of the split; default 50/50

    // Splitter-handle click detection: a press arms the gesture, any move past
    // the drag threshold latches m_handleDragged (a drag, even one that returns
    // to its start, is never a click), and only a clean release toggles.
    bool   m_handlePressed = false;
    bool   m_handleDragged = false;
    QPoint m_handlePressPos;
};

#endif // MAINWINDOW_H
