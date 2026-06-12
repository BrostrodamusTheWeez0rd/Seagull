#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QWidget>
#include <QUrl>
#include <QString>
#include <QPixmap>
#include <QPoint>
#include <QList>
#include <QElapsedTimer>

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

    // Shorts-feed behaviour (YouTube style): the short loops at the end instead
    // of dropping into ended mode, and wheel-scrolling over the video emits
    // shortsScrolled so the feed owner can advance. Cleared on every new media;
    // the orchestrator re-enables it after starting a short.
    void setShortsMode(bool on);
    bool shortsMode() const { return m_shortsMode; }

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
    void onRecordingStopped(const QString& filePath, bool ok);
    void onClipFinished(const QString& filePath, bool ok); // VOD clip done (or cancelled)

    void shareLink(); // copy the source URL to the clipboard (shell's Share button)

signals:
    void mediaEnded();
    void skipRequested(int delta);
    void shortsScrolled(int step); // shorts mode wheel: +1 = next short, -1 = previous
    void probeQualitiesRequested(const QString& url);
    // freshResolve = true bypasses the worker's metadata cache (stale-URL refetch).
    void streamUrlRequested(const QString& url, const QString& formatId, bool freshResolve);

    void fullscreenToggleRequested(); // host performs the actual window fullscreen
    void playbackStarted();           // host shows/sizes the video area
    void closed();                    // host hides the video area / leaves fullscreen

    // Drive the shell's dynamic Description tab + floating Share button: full
    // metadata when the probe reports it, empty values when media has none or
    // playback resets/tears down.
    void videoInfoChanged(const QString& title, const QString& uploader,
        const QString& views, const QString& date, const QString& description);
    void shareAvailableChanged(bool available);

    // Recording (handled by the orchestrator's SgRecorder), dispatched by source type:
    //  - live: ffmpeg -c copy (recordStart / recordStop).
    //  - VOD + local: clip the watched range [markStart, markEnd]. The already-resolved
    //    CDN URLs feeding VLC (or the local file path) ride along so the recorder can
    //    cut the section directly with ffmpeg; pageUrl is the fallback + Referer for
    //    streams, empty for a local file.
    void recordStartRequested(const QUrl& videoUrl, const QUrl& audioUrl, const QString& referer, const QString& title);
    void recordStopRequested();
    void recordClipRequested(const QString& pageUrl, const QUrl& videoUrl, const QUrl& audioUrl,
        qint64 startMs, qint64 endMs, const QString& title);
    void recordClipCancelRequested();

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
    // Z-order guards for the top-level overlay windows: true when the top-level
    // window at the point is the player's own surface (our window or one of the
    // overlays). Covering Seagull windows (dialogs, torn-off tabs) and other
    // applications' windows fail the test, so show/raise paths skip stacking
    // the overlays above them.
    bool overlaySurfaceExposedAt(const QPoint& globalPos) const;
    bool videoAreaExposed() const; // exposure probe at the video frame's centre

    void showPosterOverlay();
    void hidePosterOverlay();
    void onPlaybackError();
    void showStreamFailed(); // pin the "stream failed — replay" message + ended mode
    void closePlayer();

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
    QUrl    m_currentLocalUrl;       // local file currently playing (clip source)
    bool    m_recording = false;     // live ffmpeg capture is running

    // VOD/local recording captures the watched range: 1st Record press marks the start (button
    // pulses), 2nd press marks the end and saves [start,end] in the BACKGROUND (button
    // returns to idle). m_clipBusy guards against a second clip while one is still saving.
    bool    m_clipMarking = false;
    bool    m_clipBusy = false;
    bool    m_clipCancelled = false; // we asked for the cancel — don't report it as a failure
    bool    m_resumeAfterClip = false; // playback was paused for the grab — resume on finish
    qint64  m_clipStartMs = 0;

    // Brief "saved" confirmation pinned in the banner after a recording/clip lands;
    // keeps hideOSD from tearing the title bar down mid-message.
    bool    m_bannerNotice = false;
    void    showBannerNotice(const QString& text);
    void    restoreBannerTitle(); // put the playing media's title back

    void    toggleRecording(); // record button handler: dispatch by source type
    void    stopRecordingIfActive(); // auto-stop on stop/close/new-media/end

    // Metadata for the Info modal (filled by the probe's videoInfoReady).
    QString m_infoTitle, m_infoUploader, m_infoViews, m_infoDate, m_infoDescription;

    bool m_isStreaming = false;   // current media is an online stream (not a local file)
    bool m_isLive = false;        // online stream is live (vs VOD) — picks the record method
    bool m_streamRetried = false; // one stale-URL refetch has been spent for this stream

    // Shorts feed: wheel deltas accumulate to one notch (trackpads stream tiny
    // steps), rate-limited so a flick advances exactly one short.
    bool          m_shortsMode = false;
    int           m_shortsWheelAccum = 0;
    QElapsedTimer m_shortsScrollClock;
};

#endif // VIDEOPLAYER_H
