#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

class QSplitter;
class QTabWidget;
class QToolButton;
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

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override; // docks all floating tabs first

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
    QWidget* makeTabCloseButton(QWidget* wrapper); // small round x for one tab
    void positionPlusButton();      // snug the "+" against the last tab
    void schedulePlusReposition();  // ...after the pending layout pass settles

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

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QHash<QWidget*, QWidget*> m_tabPages; // inner page widget -> its QScrollArea wrapper
    QList<TabInfo> m_tabOrder;            // every registered tab, in addTab order
    QToolButton* m_plusBtn = nullptr;     // "+" corner button (reopen menu)
    QMenu* m_plusMenu = nullptr;
    QList<FloatingTab*> m_floating;       // torn-off tabs currently in own windows
    QWidget* m_pressedWrapper = nullptr;  // tab page under the mouse press (tear-off)
    QWidget* m_busyTab = nullptr;         // page currently showing the spinner, if any
    VideoPlayer* videoPlayer = nullptr; // hosted; owned by the widget tree
    bool m_wasMaximized = false;        // window state before going fullscreen
    double m_videoSplitRatio = 0.5;     // video fraction of the split; default 50/50
};

#endif // MAINWINDOW_H
