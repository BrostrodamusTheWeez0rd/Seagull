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
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"

struct StreamOption;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void addTab(QWidget* tab, const QString& label);

signals:
    void mediaEnded();
    void skipRequested(int delta);
    void probeQualitiesRequested(const QString& url);
    void streamUrlRequested(const QString& url, const QString& formatId);
    void streamFormatChanged(const QString& formatId);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

public slots:
    void playLocalFile(const QUrl& url);
    void playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl = QUrl(), const QUrl& cdnAudioUrl = QUrl(), const QString& title = QString());
    void showOSD();
    void hideOSD();

    void handleAvailableQualities(const QList<StreamOption>& options);
    void onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);

private slots:
    void checkMouseMovement();
    void updateOverlayPosition();
    void onSingleClickTimeout();
    void scheduleUpdateOverlay();
    void closePlayer();
    void handleStopRequest();
    void onMediaEndReached();
    void changeStreamQuality(const QString& formatId);

private:
    void installFilterRecursive(QObject* obj, QObject* filter);

    QSplitter* mainSplitter;
    QTabWidget* tabs;
    QWidget* videoContainer;

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

    QUrl currentBaseUrl;
    QString currentVideoTitle;
    QString lastRequestedFormatId; // <--- This was the missing piece!

    bool isClosing;
    qint64 savedStreamTimestamp = -1;
};

#endif