#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include <QHash>

class QSplitter;
class QTabWidget;
class QKeyEvent;
class QResizeEvent;
class QMoveEvent;
class VideoPlayer;

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

private:
    void installFilterRecursive(QObject* obj, QObject* filter);
    void enterFullScreen();
    void exitFullScreen();
    void toggleFullScreen();
    void applyStoredSplit(); // size the splitter to the remembered video/tabs ratio
    void captureSplit();     // remember the current ratio (persisted to config.ini)

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QHash<QWidget*, QWidget*> m_tabPages; // inner page widget -> its QScrollArea wrapper
    QWidget* m_busyTab = nullptr;         // page currently showing the spinner, if any
    VideoPlayer* videoPlayer = nullptr; // hosted; owned by the widget tree
    bool m_wasMaximized = false;        // window state before going fullscreen
    double m_videoSplitRatio = 0.5;     // video fraction of the split; default 50/50
};

#endif // MAINWINDOW_H
