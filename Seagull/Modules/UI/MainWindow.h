#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QSplitter>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QUrl>
#include <QEvent>
#include <QTimer>
#include <QPoint>
#include <QKeyEvent>
#include <QByteArray>

#include "Library.h"
#include "Downloads.h"
#include "Search.h"
#include "Settings.h"
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"

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
    void playVideo(const QUrl& fileUrl, const QString& title = QString());
    void showOSD();
    void hideOSD();

private slots:
    void checkMouseMovement();
    void updateOverlayPosition();
    void onSingleClickTimeout();
    void scheduleUpdateOverlay();

private:
    void installFilterRecursive(QObject* obj, QObject* filter);

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QWidget* videoContainer;
    QMediaPlayer* mediaPlayer;
    QAudioOutput* audioOutput;
    QVideoWidget* videoWidget;

    PlayerControls* playerControls;
    PlayerTitleBar* titleBar;
    QTimer* osdTimer;
    QTimer* mouseTrackerTimer;
    QTimer* clickTimer;
    QTimer* updateOverlayTimer;
    QPoint lastMousePos;
};
#endif