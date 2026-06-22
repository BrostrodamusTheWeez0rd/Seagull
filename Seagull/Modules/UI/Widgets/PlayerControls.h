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
class QGraphicsOpacityEffect;
class QVariantAnimation;

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
    void setVisualizerMode(bool on);     // audio: show the visualizer toggle button
    void setVisualizerActive(bool on);   // visualizer on/off — gates the hover-reveal triangles
    void setBaseWidth(int width);        // set the control pill width (Small/Medium/Large)
    // Canonical Progress bar size -> pill width (px) map, shared by Settings + VideoPlayer.
    static int widthForSize(const QString& size);

    // Keyboard volume control: nudge the slider by delta (clamped 0..100), which
    // cascades through setVolume to the engine + the saved setting (same path the
    // volume-popup wheel uses).
    void nudgeVolume(int delta);

public slots:
    void setAvailableQualities(const QList<StreamOption>& options);
    void toggleMute(); // M key + the mute button

signals:
    void fullscreenRequested();
    void visualizerRequested();          // audio visualizer button clicked (toggle on/off)
    void visualizerCycleRequested(int delta); // -1 prev / +1 next visualizer (side triangles)
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
    QPushButton* prevBtn;
    QPushButton* nextBtn;
    QPushButton* muteBtn;
    QPushButton* recordBtn;        // toggles recording of the current media

    QPushButton* qualityBtn;
    QFrame* qualityFrame;
    QFrame* qualityContentFrame;
    QTimer* qualityHideTimer;

    QPushButton* popoutBtn;     // detach the player into its own window
    QPushButton* prevVizBtn;    // audio-only: small triangle, previous visualizer
    QPushButton* visualizerBtn; // audio-only: toggle the visualizer
    QPushButton* nextVizBtn;    // audio-only: small triangle, next visualizer
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
    bool m_visualizerMode = false; // audio: the visualizer button is shown
    bool m_vizActive = false;      // visualizer currently on (triangles may hover-reveal)
    QTimer* vizTriHideTimer = nullptr; // grace delay before fading the cycle triangles out
    // The triangles keep their layout space (so revealing them never shifts the
    // buttons) and fade via opacity instead.
    QGraphicsOpacityEffect* m_prevVizFx = nullptr;
    QGraphicsOpacityEffect* m_nextVizFx = nullptr;
    QVariantAnimation* m_vizFade = nullptr;
    int m_barCenterX = 0; // locked screen centre, so symmetric viz growth has no drift
    void fadeVizTriangles(bool in);

    // --- Bar sizing -------------------------------------------------------------
    // The control pill's width is a static, user-chosen size (Settings -> Display ->
    // Progress bar size). A wider bar hands its extra pixels to the seeker (the row's
    // only stretch item) for finer scrubbing. setBaseWidth resizes the pill; the
    // parent (VideoPlayer::repositionOverlays) re-centres it over the video.
    QFrame* m_pillFrame = nullptr;       // the visible bar, positioned inside the window
    int  m_baseWidth = 500;              // control pill width (Small default); set via setBaseWidth
    int  m_vizExtra = 0;                 // per-triangle width currently contributed (0..22)
    bool m_resumeAfterSeek = false;      // was playing when the scrubber was grabbed
    void layoutBar();                    // place the window + pill (rest / viz width) from state

    // Floating "where will this take me" time tag: follows the cursor while hovering
    // the bar, locks to the handle while dragging. A translucent container frame
    // wraps the styled label so the pill's corners stay clean over the video.
    QFrame* seekTooltip = nullptr;
    QLabel* seekTooltipText = nullptr;
    void moveSeekTooltip(int sliderX, qint64 valueMs);  // set text + reposition + show
    void showSeekTooltipAtCursor(int cursorX);          // hover: time under the cursor
    void showSeekTooltipAtHandle(int value);            // drag: time at the handle
    double m_pulsePhase = 0.0;        // animates the recording pulse
    QTimer* recordPulseTimer = nullptr;

    QTimer* prevClickTimer;
    QTimer* nextClickTimer;

    QString formatTime(qint64 ms);
};

#endif