#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QSplitter>
#include <QWidget>
#include <QFrame>
#include <QUrl>
#include <QEvent>
#include <QTimer>
#include <QPoint>
#include <QKeyEvent>
#include <QByteArray>
#include <memory>
#include <vlcpp/vlc.hpp>
#include "Library.h"
#include "Downloads.h"
#include "Search.h"
#include "Settings.h"
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"
#include "../Backend/SgYtDlp.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

public slots:
    void playLocalFile(const QUrl& url); // New dedicated slot
    void playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl = QUrl(), const QUrl& cdnAudioUrl = QUrl(), const QString& title = QString());
    void showOSD();
    void hideOSD();

private slots:
    void checkMouseMovement();
    void updateOverlayPosition();
    void onSingleClickTimeout();
    void scheduleUpdateOverlay();
    void closePlayer();
    void handleStopRequest();

    // --- Stream Quality Routing ---
    void handleAvailableQualities(const QList<StreamOption>& options);
    void changeStreamQuality(const QString& formatId);
    void onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);

private:
    void installFilterRecursive(QObject* obj, QObject* filter);

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QWidget* videoContainer;

    // VLC Core Components
    std::shared_ptr<VLC::Instance> vlcInstance;
    std::shared_ptr<VLC::MediaPlayer> vlcPlayer;
    QFrame* videoWidget;

    PlayerControls* playerControls;
    PlayerTitleBar* titleBar;
    QTimer* osdTimer;
    QTimer* mouseTrackerTimer;
    QTimer* clickTimer;
    QTimer* updateOverlayTimer;
    QPoint lastMousePos;

    // --- Stream State Management ---
    SgYtDlp* streamBackend;
    QUrl currentBaseUrl;
    QString currentVideoTitle;

    // Safety guard to prevent processing events during destruction
    bool isClosing;

    qint64 savedStreamTimestamp = -1;
};

#endif