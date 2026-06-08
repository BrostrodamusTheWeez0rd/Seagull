#ifndef PLAYERCONTROLS_H
#define PLAYERCONTROLS_H

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTime>
#include <QEvent>
#include <QTimer>
#include <QMoveEvent>
#include <QFrame>
#include <QGraphicsColorizeEffect>
#include <QHideEvent>
#include <QSettings>
#include <vlcpp/vlc.hpp>
#include "../../Backend/SgYtDlp.h"

class PlayerControls : public QWidget {
    Q_OBJECT

public:
    explicit PlayerControls(VLC::MediaPlayer* player, QWidget* parent = nullptr);
    ~PlayerControls();

    void resetUiState();
    void updateVolumePosition();
    void setVolumeUi(int volume);
    void setStreamingMode(bool isStream);
    void applyAudioState();
    bool hasOpenPopup() const; // true while the volume or quality popup is showing

    void stopPolling();
    void startPolling();
    void setEndedMode(bool ended); // Freeze seeker/time at end-of-stream
    void setCurrentFormat(const QString& formatId); // Track active selection

public slots:
    void setAvailableQualities(const QList<StreamOption>& options);

signals:
    void fullscreenRequested();
    void stopRequested();
    void replayRequested();
    void qualitySelected(QString formatId);

    // delta = +1 for next, -1 for prev
    void skipRequested(int delta);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private slots:
    void pollVlcState();
    void seek(int position);
    void togglePlayback();
    void toggleMute();
    void setVolume(int volume);
    void hideVolumeFrame();
    void hideQualityFrame();
    void resetMuteButton();

    void onPrevSingleClick();
    void onNextSingleClick();

private:
    VLC::MediaPlayer* m_player;
    QTimer* uiPollTimer;
    QSettings m_settings;
    QString m_currentFormatId; // Active track state
    QList<StreamOption> m_lastOptions; // Cache the list to redraw on demand

    QPushButton* playPauseBtn;
    QPushButton* stopBtn;
    QPushButton* prevBtn;
    QPushButton* nextBtn;
    QPushButton* muteBtn;

    QPushButton* qualityBtn;
    QFrame* qualityFrame;
    QFrame* qualityContentFrame;
    QTimer* qualityHideTimer;

    QPushButton* fullscreenBtn;
    QSlider* positionSlider;

    QFrame* volumeFrame;
    QFrame* contentFrame;
    QSlider* volumeSlider;
    QTimer* volumeHideTimer;
    QTimer* volumeTextTimer;

    QLabel* timeLabel;
    qint64 m_duration;
    bool isUserSeeking;
    bool m_endedMode = false; // true after EOF until the next media starts

    QTimer* prevClickTimer;
    QTimer* nextClickTimer;

    QString formatTime(qint64 ms);
};

#endif