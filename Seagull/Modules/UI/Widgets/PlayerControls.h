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

    void updateVolumePosition();
    void setVolumeUi(int volume);
    void setStreamingMode(bool isStream);
    void applyAudioState();
    void stopPolling();

public slots:
    void setAvailableQualities(const QList<StreamOption>& options);

signals:
    void fullscreenRequested();
    void stopRequested();
    void qualitySelected(QString formatId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void pollVlcState();
    void seek(int position);
    void togglePlayback();
    void toggleMute();
    void setVolume(int volume);
    void hideVolumeFrame();
    void hideQualityFrame();
    void resetMuteButton();

private:
    VLC::MediaPlayer* m_player;
    QTimer* uiPollTimer;
    QSettings m_settings;

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

    QString formatTime(qint64 ms);
};
#endif