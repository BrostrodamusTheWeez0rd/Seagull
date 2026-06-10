#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QWidget>
#include <QUrl>
#include <QString>
#include <QPixmap>
#include <QPoint>
#include <QList>

class QFrame;
class QLabel;
class QTimer;
class QNetworkAccessManager;
class PlaybackEngine;
class PlayerControls;
class PlayerTitleBar;
struct StreamOption;

// The video feature: owns the VLC render surface (via PlaybackEngine), the
// overlays (controls / title / poster), the OSD and mouse handling. MainWindow
// just hosts this widget; the orchestrator wires it to the modules and workers.
// Window-level concerns (fullscreen, repositioning the top-level overlays when
// the window moves) are delegated back to the host via signals/public methods.
class VideoPlayer : public QWidget {
    Q_OBJECT
public:
    explicit VideoPlayer(QWidget* parent = nullptr);

    // Called by the host (MainWindow) for genuinely window-level events.
    void repositionOverlays(); // keep the top-level overlays glued to the video
    void raiseOverlays();      // re-stack overlays above the video (after fullscreen)
    void togglePlayPause();    // space-bar / single-click handler entry point

public slots:
    void playLocalFile(const QUrl& url);
    void playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl = QUrl(), const QUrl& cdnAudioUrl = QUrl(), const QString& title = QString());
    void showOSD();
    void hideOSD();

    void handleAvailableQualities(const QList<StreamOption>& options);
    void onThumbnailResolved(const QString& thumbUrl);
    void onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);
    void onVideoInfo(const QString& title, const QString& uploader,
        const QString& views, const QString& date, const QString& description);
    void onLiveStatus(bool isLive); // probe reported live/VOD — drives the LIVE seeker

    // Recorder state, pushed back from the orchestrator's SgRecorder.
    void onRecordingStarted();
    void onRecordingStopped();

signals:
    void mediaEnded();
    void skipRequested(int delta);
    void probeQualitiesRequested(const QString& url);
    void streamUrlRequested(const QString& url, const QString& formatId);

    void fullscreenToggleRequested(); // host performs the actual window fullscreen
    void playbackStarted();           // host shows/sizes the video area
    void closed();                    // host hides the video area / leaves fullscreen

    // Live-stream recording (handled by the orchestrator's SgRecorder).
    void recordStartRequested(const QUrl& videoUrl, const QUrl& audioUrl, const QString& referer, const QString& title);
    void recordStopRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // keep overlays glued on any resize

private slots:
    void checkMouseMovement();
    void onSingleClickTimeout();
    void onMediaEndReached();
    void handleStopRequest();
    void handleReplay();
    void changeStreamQuality(const QString& formatId);

private:
    void showPosterOverlay();
    void hidePosterOverlay();
    void onPlaybackError();
    void showStreamFailed(); // pin the "stream failed — replay" message + ended mode
    void closePlayer();
    void showInfoModal();    // pop the playing video's metadata + description
    void shareLink();        // copy the source URL to the clipboard

    PlaybackEngine* engine;
    QFrame* videoWidget;

    PlayerControls* playerControls;
    PlayerTitleBar* titleBar;

    // Poster shown over the video when paused / at end-of-stream. A separate
    // top-level window (like the other overlays) because the VLC HWND draws over
    // child widgets; click-through so play/pause on the video still works.
    QLabel* posterOverlay;
    QPixmap m_posterPixmap;
    QNetworkAccessManager* m_thumbNam;

    QTimer* osdTimer;
    QTimer* mouseTrackerTimer;
    QTimer* clickTimer;
    QTimer* updateOverlayTimer;
    QTimer* retryTimer;        // bounds the stale-URL refetch so it can't hang forever
    QPoint  lastMousePos;

    QUrl    currentBaseUrl;
    QString currentVideoTitle;
    QString lastRequestedFormatId;
    qint64  savedStreamTimestamp = -1;

    // The resolved stream URLs currently feeding VLC — handed to the recorder so it
    // captures the exact same (ad-free, for Twitch) stream. For Twitch this is the
    // local proxy URL. Cleared when playback stops.
    QUrl    m_recordVideoUrl;
    QUrl    m_recordAudioUrl;
    bool    m_recording = false;
    void    toggleRecording(); // record button handler: emits start or stop
    void    stopRecordingIfActive(); // auto-stop on stop/close/new-media/end

    // Metadata for the Info modal (filled by the probe's videoInfoReady).
    QString m_infoTitle, m_infoUploader, m_infoViews, m_infoDate, m_infoDescription;

    bool m_isStreaming = false;   // current media is an online stream (not a local file)
    bool m_streamRetried = false; // one stale-URL refetch has been spent for this stream
};

#endif // VIDEOPLAYER_H
