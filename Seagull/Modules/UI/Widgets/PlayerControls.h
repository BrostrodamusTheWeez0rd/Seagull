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
#include <QIcon>
#include <QStyle>
#include <QHideEvent>
#include <QSettings>
#include <QColor>
#include <QList>
#include "../../Backend/SgYtDlp.h"

class PlaybackEngine;

class PlayerControls : public QWidget {
    Q_OBJECT

public:
    explicit PlayerControls(PlaybackEngine* engine, QWidget* parent = nullptr);
    ~PlayerControls();

    void resetUiState();
    void updateVolumePosition();
    void setVolumeUi(int volume);
    void setStreamingMode(bool isStream);
    void applyAudioState();
    bool hasOpenPopup() const;     // true while the volume or quality popup is showing
    bool popupUnderCursor() const; // the cursor is on an open popup (mid-interaction)
    void closePopups();            // shut both popups (they close before the bar fades)

    void stopPolling();
    void startPolling();
    void setEndedMode(bool ended); // Freeze seeker/time at end-of-stream
    void setLiveMode(bool isLive); // Live stream: show "LIVE", disable seeking
    void setCurrentFormat(const QString& formatId); // Track active selection
    void setRecording(bool on);          // reflect recorder state: red pulse while on
    void setRecordAvailable(bool avail); // show/hide the Record button (any playing media)
    void setPoppedOut(bool popped);      // swap the pop-out button's glyph/tooltip

public slots:
    void setAvailableQualities(const QList<StreamOption>& options);

signals:
    void fullscreenRequested();
    void popoutRequested(); // pop-out button clicked (detach to / re-dock from its own window)
    void stopRequested();
    void replayRequested();
    void qualitySelected(QString formatId);
    void recordToggleRequested(); // Record button clicked (start if idle, stop if recording)

    // delta = +1 for next, -1 for prev
    void skipRequested(int delta);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void changeEvent(QEvent* event) override; // re-tint icons on theme/palette change

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
    // The glyph tint on the round buttons can't be set via stylesheet, so it's
    // pulled from the themed palette and refreshed on every palette change.
    QIcon  tintIcon(const QIcon& src, const QColor& col) const;          // flat-tint helper
    QColor iconColorFor(QPushButton* btn) const;                        // idle/hover tint for a button
    QIcon  makeIcon(const QString& resourcePath, QPushButton* btn) const; // tinted SVG/resource glyph
    void   retintIcon(QPushButton* btn, const QColor& col);             // recolour current glyph
    void   refreshIconTints();

    PlaybackEngine* m_engine;
    QTimer* uiPollTimer;
    QSettings m_settings;
    QString m_currentFormatId; // Active track state
    QList<StreamOption> m_lastOptions; // Cache the list to redraw on demand

    QList<QPushButton*> m_iconButtons; // the round buttons sharing the colorize tint
    QColor m_iconIdle;  // tint when not hovered (palette WindowText)
    QColor m_iconHover; // tint when hovered (palette HighlightedText)

    QPushButton* playPauseBtn;
    QPushButton* stopBtn;
    QPushButton* prevBtn;
    QPushButton* nextBtn;
    QPushButton* muteBtn;
    QPushButton* recordBtn;        // toggles recording of the current media

    QPushButton* qualityBtn;
    QFrame* qualityFrame;
    QFrame* qualityContentFrame;
    QTimer* qualityHideTimer;

    QPushButton* popoutBtn;     // detach the player into its own window
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
    bool m_isLive = false;    // live stream: no meaningful duration, no seeking
    bool m_recording = false; // recorder is running (drives the red pulse)
    double m_pulsePhase = 0.0;        // animates the recording pulse
    QTimer* recordPulseTimer = nullptr;

    QTimer* prevClickTimer;
    QTimer* nextClickTimer;

    QString formatTime(qint64 ms);
};

#endif