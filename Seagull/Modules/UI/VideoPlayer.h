#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <QWidget>
#include <QUrl>
#include <QString>
#include <QPixmap>
#include <QImage>
#include <QPoint>
#include <QList>
#include <QVector>
#include <QElapsedTimer>

class QFrame;
class QLabel;
class QPushButton;
class QPropertyAnimation;
class QTimer;
class QNetworkAccessManager;
class PlaybackEngine;
class PlayerControls;
class PlayerTitleBar;
class Visualizer;
struct StreamOption;

// What kind of media is loaded. Drives the surface (VLC video frame / album art
// poster / still image) and which chrome shows. Audio keeps the art poster up
// full-time and swaps fullscreen for a visualizer button; photo skips VLC
// entirely and shows the image with just two fading side arrows.
enum class MediaKind { Video, Audio, Photo };

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
    void suppressOverlaysForTransition();  // hide overlays before a fullscreen/maximize transition
    void showOverlaysAfterTransition();    // un-suppress, reposition, and raise overlays after the transition
    void togglePlayPause();    // space-bar / single-click handler entry point
    void applyVisualizerSettings(); // re-read the visualizer config (settings changed)
    void setSeekBarSize(int width); // Display "Progress bar size" -> control pill width
    void setVisualizerSuspended(bool on); // pause the visualizer's render timer while another module hammers the GUI thread (e.g. Library build)

    // Keyboard transport (driven by the host's key handling):
    void seekRelative(qint64 deltaMs); // arrow keys: nudge ±5s, clamped to [0,len]
    void stepFrame(int dir);           // comma/period: one frame back/forward, paused only
    void changeVolume(int delta);      // up/down arrows: nudge volume, clamped 0..100
    void toggleMute();                 // M key: toggle mute
    bool hasActiveMedia() const;       // is there media loaded to act on?

    // Equalizer: the EQ tab edits + persists per-kind presets; these apply them.
    // applyEqualizer/disableEqualizer forward to the engine (used for a live edit
    // when the playing kind matches the edited type). currentMediaKind() lets the
    // orchestrator gate those live edits; the saved EQ for the current media is
    // applied automatically on play (see applyEqualizerForCurrentKind).
    void applyEqualizer(const QVector<float>& gains, float preampDb);
    void disableEqualizer();
    // Normalization (peak protection): the EQ tab toggles it per-kind. This forwards a
    // live edit to the engine when the playing kind matches; the saved per-kind state is
    // applied automatically on play (see applyNormalizationForCurrentKind).
    void setNormalizationEnabled(bool on);
    MediaKind currentMediaKind() const { return m_kind; }
    qint64 mediaPosition() const;      // current play position (ms), for the SMTC timeline
    qint64 mediaDuration() const;      // media length (ms), for the SMTC timeline

    // Shorts-feed behaviour (YouTube style): the short loops at the end instead
    // of dropping into ended mode, and wheel-scrolling over the video emits
    // shortsScrolled so the feed owner can advance. Cleared on every new media;
    // the orchestrator re-enables it after starting a short.
    void setShortsMode(bool on);
    bool shortsMode() const { return m_shortsMode; }

    // The shell pushes the tabs-pane state so the splitter-toggle chevron
    // points the way the pane will move: down to drop it, up to bring it back.
    void setTabsPaneOpen(bool open);

    // Pop-out: the shell moves this whole widget between the main window's
    // splitter and its own top-level window. setPoppedOut updates the controls'
    // button and suppresses the tabs chevron (no shared splitter when floating);
    // rebindOutputWindow re-attaches VLC to the render frame's new HWND, which
    // Qt recreates whenever the widget changes top-level windows.
    void setPoppedOut(bool popped);
    void rebindOutputWindow();
    void reownOverlays(); // re-point the overlay tool windows' owner at the current host window
    void hardStop();      // full teardown: release media + emit closed (pop-out window close)

public slots:
    void playLocalFile(const QUrl& url);
    void playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl = QUrl(), const QUrl& cdnAudioUrl = QUrl(), const QString& title = QString());
    void showOSD();
    void hideOSD();

    void handleAvailableQualities(const QList<StreamOption>& options);
    void onThumbnailResolved(const QString& thumbUrl);
    // Local-file poster (frame grab / cover art) answered by the orchestrator's
    // SgThumbnailer; used by the stop/EOF poster like a stream's thumbnail.
    void onLocalPosterReady(const QString& filePath, const QPixmap& pixmap);
    void onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl);
    void onVideoInfo(const QString& title, const QString& uploader,
        const QString& views, const QString& date, const QString& description);
    void onLiveStatus(bool isLive); // probe reported live/VOD — drives the LIVE seeker
    void onCommentCount(int count); // probe reported comment_count — announce a Comments tab

    // Recorder state, pushed back from the orchestrator's SgRecorder.
    void onRecordingStarted();
    void onRecordingStopped(const QString& filePath, bool ok);
    void onClipFinished(const QString& filePath, bool ok); // VOD clip done (or cancelled)

    void shareLink(); // copy the source URL to the clipboard (shell's Share button)

signals:
    void mediaEnded();
    void skipRequested(int delta);

    // Mirror playback out to the Windows media controls (SMTC). State maps to
    // SgMediaControls::Status: 0 = stopped, 1 = playing, 2 = paused.
    void smtcStateChanged(int state);
    void smtcMetadata(const QString& title, const QString& artist, bool isVideo);
    void smtcArtwork(const QImage& image);
    void shortsScrolled(int step); // shorts mode wheel: +1 = next short, -1 = previous
    void probeQualitiesRequested(const QString& url);
    // freshResolve = true bypasses the worker's metadata cache (stale-URL refetch).
    void streamUrlRequested(const QString& url, const QString& formatId, bool freshResolve);

    void fullscreenToggleRequested(); // host performs the actual window fullscreen
    void visualizerRequested();       // audio visualizer button (unhooked for now)
    void popOutRequested();           // host detaches/re-docks the player window
    void playbackStarted();           // host shows/sizes the video area
    void closed();                    // host hides the video area / leaves fullscreen
    void tabsToggleRequested();       // splitter-toggle chevron clicked — host toggles the pane
    void localPosterRequested(const QString& filePath); // thumbnailer answers via onLocalPosterReady

    // Drive the shell's dynamic Description tab + floating Share button: full
    // metadata when the probe reports it, empty values when media has none or
    // playback resets/tears down.
    void videoInfoChanged(const QString& title, const QString& uploader,
        const QString& views, const QString& date, const QString& description);
    void shareAvailableChanged(bool available);

    // An online VOD resolved and reports `commentCount` comments: the shell offers a
    // dynamic Comments tab and fetches lazily (only when viewed). Fired once per
    // distinct page URL (the probe re-reports on quality switches).
    void commentsAvailable(const QString& pageUrl, int commentCount);

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
    void applyPosterPixmap(const QPixmap& pm); // stream thumbnail decoded (direct or via ffmpeg)
    void updateSplitterToggle(const QPoint& globalPos); // show/hide the chevron by cursor proximity
    void positionSplitterToggle();                      // bottom-centre, perched above the pill when it's up
    void onPlaybackError();
    void showStreamFailed(); // pin the "stream failed — replay" message + ended mode
    void closePlayer();
    void applyEqualizerForCurrentKind(); // load + apply Eq/<Audio|Video>/* for m_kind on play
    void applyNormalizationForCurrentKind(); // load + apply Eq/<kind>/NormEnabled for m_kind on play

    // Content kind + its presentation helpers.
    MediaKind m_kind = MediaKind::Video;
    static MediaKind kindForLocalFile(const QUrl& url);
    void applyKindChrome();          // configure the controls bar for m_kind (visualizer swap)
    void showAudioArt();             // poster up full-time: cover art, or the placeholder
    QPixmap audioPlaceholder();      // lazily-rendered music-note placeholder
    void openPhoto(const QUrl& url); // load + display a still image (no VLC playback)
    QPixmap m_audioPlaceholder;

    // Audio visualizer: a top-level overlay (seagull sky) shown over the audio
    // surface when the visualizer button is toggled on, replacing the album art.
    void toggleVisualizer();
    void cycleVisualizer(int delta); // switch to prev/next visualizer + persist
    Visualizer* visualizer = nullptr;
    bool m_visualizerActive = false;
    bool m_killGullsOnEnd = true; // gulls fall at end of song (Settings)

    // Photo viewer: large prev/next arrows glued to the image's left/right edges,
    // fading on the same OSD clock as the controls. Top-level like the overlays.
    QPushButton* prevPhotoBtn = nullptr;
    QPushButton* nextPhotoBtn = nullptr;
    QPropertyAnimation* prevPhotoFade = nullptr;
    QPropertyAnimation* nextPhotoFade = nullptr;

    PlaybackEngine* engine;
    QFrame* videoWidget;

    PlayerControls* playerControls;
    PlayerTitleBar* titleBar;

    // Progress bar size (Settings -> Display) is a TARGET width; the actual bar width
    // is this clamped to the video frame so the pill never spills past the video edges.
    // Re-applied on every reposition (resize / move / show), so it tracks the window.
    int  m_seekBarTargetWidth = 500; // chosen size in px (Small default)
    void applySeekBarWidth();        // clamp the target to the video width and apply it

    // OSD fades: controls and banner ease in/out on the same clock as the
    // splitter chevron (kOverlayFadeInMs/kOverlayFadeOutMs). Event-driven
    // "pinned" shows (EOF, stream failed, banner notices) bypass the fade via
    // pinOverlayWindow so a mid-fade hide can't swallow them.
    QPropertyAnimation* controlsFade = nullptr;
    QPropertyAnimation* titleFade = nullptr;
    void fadeOverlayWindow(QWidget* w, QPropertyAnimation* anim, bool in);
    void pinOverlayWindow(QWidget* w, QPropertyAnimation* anim); // instant, full opacity

    // Circular chevron near the splitter (YouTube-style): appears when the
    // cursor nears the bottom of the video (not over the controls pill), sits
    // underneath the controls (which nudge up to clear it), and toggles the
    // tabs pane. Lingers briefly after the cursor leaves the trigger zone so
    // it can actually be reached and clicked. Top-level like the other overlays.
    QPushButton* splitterToggleBtn = nullptr;
    QTimer* splitterBtnHideTimer = nullptr;
    QPropertyAnimation* splitterBtnFade = nullptr; // windowOpacity fade in/out
    void fadeSplitterToggle(bool in);
    bool m_tabsPaneOpen = true;
    bool m_poppedOut = false; // player is in its own window — no tabs chevron

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
    QString m_commentsRequestedUrl; // page URL we last announced comments for (dedup across re-probes)

    bool m_isStreaming = false;   // current media is an online stream (not a local file)
    bool m_isLive = false;        // online stream is live (vs VOD) — picks the record method
    bool m_streamRetried = false; // one stale-URL refetch has been spent for this stream
    bool m_fetching = false;      // resolving/loading a stream — poster stands in for the video
    bool m_stopped = false;       // stopped/ended, replay-ready: play = replay, next Stop = teardown

    // Shorts feed: wheel deltas accumulate to one notch (trackpads stream tiny
    // steps), rate-limited so a flick advances exactly one short.
    bool          m_shortsMode = false;
    int           m_shortsWheelAccum = 0;
    QElapsedTimer m_shortsScrollClock;
};

#endif // VIDEOPLAYER_H
