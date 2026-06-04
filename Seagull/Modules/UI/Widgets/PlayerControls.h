#ifndef PLAYERCONTROLS_H
#define PLAYERCONTROLS_H

#include <QWidget>
#include <QMediaPlayer>
#include <QAudioOutput>
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

class PlayerControls : public QWidget {
    Q_OBJECT
public:
    explicit PlayerControls(QMediaPlayer* player, QWidget* parent = nullptr);
    void updateVolumePosition();
    void setVolumeUi(int volume);

    // UI FLAG: Shows/hides the gear icon
    void setStreamingMode(bool isStream);

signals:
    void fullscreenRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void updateState(QMediaPlayer::PlaybackState state);
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    void seek(int position);
    void togglePlayback();
    void toggleMute();
    void setVolume(int volume);

    void hideVolumeFrame();
    void hideQualityFrame(); // UI FLAG: Hides the quality popup

    void resetMuteButton();
    void syncVolumeUi();

private:
    QMediaPlayer* m_player;
    QPushButton* playPauseBtn;
    QPushButton* stopBtn;
    QPushButton* prevBtn;
    QPushButton* nextBtn;
    QPushButton* muteBtn;

    // UI ELEMENTS: Gear and Popup
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

    QString formatTime(qint64 ms);
};
#endif