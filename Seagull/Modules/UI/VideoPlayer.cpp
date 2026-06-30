#include "VideoPlayer.h"
#include "../Backend/PlaybackEngine.h"
#include "../Backend/SgYtDlp.h"          // StreamOption
#include "../Backend/SgThumbnailer.h"    // decodeViaFfmpeg (WebP fallback)
#include "../Backend/SgPaths.h"
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"
#include "Widgets/Visualizer.h"

#include <QVBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QCursor>
#include <QDialog>
#include <QTextEdit>
#include <QStringList>
#include <QGuiApplication>
#include <QWindow>
#include <QClipboard>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRect>
#include <QSettings>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPainter>
#include <QSvgRenderer>
#include <QFile>

namespace {
    // One clock for every overlay fade: chevron, controls bar, banner.
    constexpr int kOverlayFadeInMs  = 150;
    constexpr int kOverlayFadeOutMs = 350;
}

VideoPlayer::VideoPlayer(QWidget* parent) : QWidget(parent) {
    // Floor for the video area so the splitter can't squeeze it away at small
    // window sizes (tunable). Width keeps the 500px controls pill from overhanging.
    setMinimumSize(560, 315);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    videoWidget = new QFrame(this);
    videoWidget->setStyleSheet("background-color: black;");
    layout->addWidget(videoWidget);

    engine = new PlaybackEngine(this);

    // VLC's output HWND is bound explicitly by the orchestrator early in
    // Seagull::run() (via rebindOutputWindow), BEFORE any startup modal. This used
    // to be a QTimer::singleShot(0) here, but that stray deferred call could fire
    // inside a modal's nested event loop if a dialog ran before the window was
    // shown, realizing the native window under an active modal block and leaving
    // the whole app input-dead. Binding it deterministically removes that landmine.

    connect(engine, &PlaybackEngine::endReached, this, &VideoPlayer::onMediaEndReached);
    // Pause deliberately shows the frozen frame (VLC keeps it on screen) — the
    // poster/thumbnail is only for EOF (replay) and the fetch placeholder.
    connect(engine, &PlaybackEngine::playing, this, [this]() {
        m_fetching = false;
        m_stopped  = false; // playing again — Stop is back to stage one
        // Audio keeps the album-art poster up the whole time; video drops it so
        // the frame shows through.
        if (m_kind == MediaKind::Audio) showAudioArt();
        else hidePosterOverlay();
        if (visualizer) { visualizer->setPaused(false); visualizer->reviveGulls(); } // resume + revive
        if (titleBar) titleBar->setLoading(false); // playback started — stop the seagull
        emit smtcStateChanged(1); // SMTC: Playing
        });
    connect(engine, &PlaybackEngine::paused, this, [this]() {
        if (visualizer) visualizer->setPaused(true); // freeze the sky/sea while paused
        emit smtcStateChanged(2); // SMTC: Paused
        });
    connect(engine, &PlaybackEngine::errorOccurred, this, &VideoPlayer::onPlaybackError);

    // Audio tap feeds the visualizer (only emits while an audio track is tapped).
    connect(engine, &PlaybackEngine::audioLevel, this, [this](float l) {
        if (visualizer) visualizer->setAudioLevel(l);
        });
    connect(engine, &PlaybackEngine::audioSpectrum, this, [this](float b, float m, float t) {
        if (visualizer) visualizer->setSpectrum(b, m, t);
        });
    connect(engine, &PlaybackEngine::beat, this, [this]() {
        if (visualizer) visualizer->beat();
        });

    updateOverlayTimer = new QTimer(this);
    updateOverlayTimer->setSingleShot(true);
    connect(updateOverlayTimer, &QTimer::timeout, this, &VideoPlayer::repositionOverlays);

    Qt::WindowFlags overlayFlags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;

    playerControls = new PlayerControls(engine, this);
    playerControls->setWindowFlags(overlayFlags);
    playerControls->setAttribute(Qt::WA_TranslucentBackground);
    // Start at the saved Progress bar size (Settings -> Display). Stored as a target;
    // applySeekBarWidth (here + on every reposition) clamps it to the video frame so
    // the bar never overhangs. Live changes arrive via setSeekBarSize.
    {
        QSettings s(SgPaths::configFile(), QSettings::IniFormat);
        m_seekBarTargetWidth = PlayerControls::widthForSize(
            s.value("Display/SeekBarSize", "Small").toString());
        applySeekBarWidth();
    }

    titleBar = new PlayerTitleBar(this);
    titleBar->setWindowFlags(overlayFlags);
    titleBar->setAttribute(Qt::WA_TranslucentBackground);

    // OSD fade animations (window opacity — these are top-level windows). A
    // completed fade-out hides the window and resets it to full opacity, so
    // every direct show() elsewhere starts from a clean, fully opaque state.
    auto makeOverlayFade = [this](QWidget* w) {
        auto* anim = new QPropertyAnimation(w, "windowOpacity", this);
        connect(anim, &QPropertyAnimation::finished, this, [w, anim]() {
            if (anim->endValue().toDouble() == 0.0) {
                w->hide();
                w->setWindowOpacity(1.0);
            }
            });
        return anim;
    };
    controlsFade = makeOverlayFade(playerControls);
    titleFade    = makeOverlayFade(titleBar);

    // Poster overlay: opaque image window that covers the video frame. Mouse
    // events pass straight through so clicking the video still toggles playback.
    posterOverlay = new QLabel(this);
    posterOverlay->setWindowFlags(overlayFlags | Qt::WindowTransparentForInput);
    posterOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    posterOverlay->setStyleSheet("background-color: black;");
    posterOverlay->setAlignment(Qt::AlignCenter);
    posterOverlay->setScaledContents(false);
    posterOverlay->hide();

    // Audio visualizer overlay (seagull sky). Same top-level, click-through idiom
    // as the poster so play/pause-on-click still works through it; shown only
    // when the audio visualizer button is toggled on.
    visualizer = new Visualizer(this);
    visualizer->setWindowFlags(overlayFlags | Qt::WindowTransparentForInput);
    visualizer->setAttribute(Qt::WA_TransparentForMouseEvents);
    visualizer->hide();
    applyVisualizerSettings(); // pick up the saved gull style

    // Splitter-toggle chevron (YouTube-style): a small circular button that
    // appears when the cursor nears the splitter (bottom strip of the video,
    // not the controls pill) and toggles the tabs pane via the shell. The
    // arrow mirrors the move the pane will make (see setTabsPaneOpen).
    splitterToggleBtn = new QPushButton(this);
    splitterToggleBtn->setWindowFlags(overlayFlags);
    splitterToggleBtn->setAttribute(Qt::WA_TranslucentBackground);
    splitterToggleBtn->setObjectName("splitterToggleButton");
    splitterToggleBtn->setFixedSize(30, 30); // same circle as the control buttons
    splitterToggleBtn->setCursor(Qt::PointingHandCursor);
    splitterToggleBtn->hide();
    setTabsPaneOpen(true);
    connect(splitterToggleBtn, &QPushButton::clicked, this, [this]() { emit tabsToggleRequested(); });

    // Leaving the trigger zone doesn't hide the chevron instantly — it lingers
    // so the cursor can travel down to it; re-entering cancels the countdown.
    splitterBtnHideTimer = new QTimer(this);
    splitterBtnHideTimer->setSingleShot(true);
    splitterBtnHideTimer->setInterval(1000);
    connect(splitterBtnHideTimer, &QTimer::timeout, this, [this]() {
        fadeSplitterToggle(false); // ease out; hidden for real when the fade lands
        });

    // Fade on the window opacity (it's a top-level overlay); reversible
    // mid-flight so a re-entry during the fade-out eases straight back in.
    splitterBtnFade = new QPropertyAnimation(splitterToggleBtn, "windowOpacity", this);
    connect(splitterBtnFade, &QPropertyAnimation::finished, this, [this]() {
        if (splitterBtnFade->endValue().toDouble() == 0.0) {
            splitterToggleBtn->hide();
            splitterToggleBtn->setWindowOpacity(1.0);
            repositionOverlays(); // the controls drop back down to the edge
        }
        });

    // Photo viewer arrows: large circular prev/next buttons glued to the image's
    // left/right edges, fading on the same OSD clock as the controls. Top-level
    // overlays like the rest (the VLC/poster surface draws over child widgets).
    auto makePhotoArrow = [this, overlayFlags](const QString& glyph) {
        auto* b = new QPushButton(glyph, this);
        b->setWindowFlags(overlayFlags);
        b->setAttribute(Qt::WA_TranslucentBackground);
        b->setObjectName("photoNavButton");
        b->setFixedSize(44, 44);
        b->setCursor(Qt::PointingHandCursor);
        b->hide();
        return b;
    };
    prevPhotoBtn = makePhotoArrow(QStringLiteral("‹")); // ‹
    nextPhotoBtn = makePhotoArrow(QStringLiteral("›")); // ›
    prevPhotoFade = makeOverlayFade(prevPhotoBtn);
    nextPhotoFade = makeOverlayFade(nextPhotoBtn);
    connect(prevPhotoBtn, &QPushButton::clicked, this, [this]() { emit skipRequested(-1); });
    connect(nextPhotoBtn, &QPushButton::clicked, this, [this]() { emit skipRequested(1); });

    m_thumbNam = new QNetworkAccessManager(this);

    osdTimer = new QTimer(this);
    osdTimer->setSingleShot(true);
    connect(osdTimer, &QTimer::timeout, this, &VideoPlayer::hideOSD);

    mouseTrackerTimer = new QTimer(this);
    connect(mouseTrackerTimer, &QTimer::timeout, this, &VideoPlayer::checkMouseMovement);
    mouseTrackerTimer->start(100);

    clickTimer = new QTimer(this);
    clickTimer->setSingleShot(true);
    connect(clickTimer, &QTimer::timeout, this, &VideoPlayer::onSingleClickTimeout);

    // If a stale-URL refetch doesn't come back with a playable stream in time,
    // give up gracefully instead of leaving "refetching..." up forever.
    retryTimer = new QTimer(this);
    retryTimer->setSingleShot(true);
    connect(retryTimer, &QTimer::timeout, this, &VideoPlayer::showStreamFailed);

    lastMousePos = QCursor::pos();
    m_shortsScrollClock.start();
    videoWidget->installEventFilter(this);

    connect(playerControls, &PlayerControls::qualitySelected, this, &VideoPlayer::changeStreamQuality);
    connect(playerControls, &PlayerControls::stopRequested, this, &VideoPlayer::handleStopRequest);
    connect(playerControls, &PlayerControls::replayRequested, this, &VideoPlayer::handleReplay);
    connect(playerControls, &PlayerControls::skipRequested, this, [this](int delta) { emit skipRequested(delta); });
    connect(playerControls, &PlayerControls::fullscreenRequested, this, [this]() { emit fullscreenToggleRequested(); });
    connect(playerControls, &PlayerControls::visualizerRequested, this, &VideoPlayer::toggleVisualizer);
    connect(playerControls, &PlayerControls::visualizerCycleRequested, this, &VideoPlayer::cycleVisualizer);
    connect(titleBar, &PlayerTitleBar::closeRequested, this, &VideoPlayer::closePlayer); // banner X = teardown
    connect(playerControls, &PlayerControls::popoutRequested, this, [this]() { emit popOutRequested(); });
    connect(playerControls, &PlayerControls::recordToggleRequested, this, &VideoPlayer::toggleRecording);
}

void VideoPlayer::setShortsMode(bool on) {
    m_shortsMode = on;
    m_shortsWheelAccum = 0;
}

void VideoPlayer::setTabsPaneOpen(bool open) {
    m_tabsPaneOpen = open;
    // Arrow shows the direction the splitter will move: pane open = clicking
    // drops it (down), pane down = clicking brings it back up.
    if (splitterToggleBtn) splitterToggleBtn->setText(open ? QStringLiteral("▼")
                                                           : QStringLiteral("▲"));
}

void VideoPlayer::setPoppedOut(bool popped) {
    m_poppedOut = popped;
    if (playerControls) playerControls->setPoppedOut(popped);
    if (popped) {
        // No shared splitter while floating — kill the chevron and its countdown.
        if (splitterBtnHideTimer) splitterBtnHideTimer->stop();
        if (splitterToggleBtn && splitterToggleBtn->isVisible()) {
            if (splitterBtnFade) splitterBtnFade->stop();
            splitterToggleBtn->hide();
            splitterToggleBtn->setWindowOpacity(1.0);
        }
    }
    repositionOverlays();
}

void VideoPlayer::rebindOutputWindow() {
    // Qt recreates the render frame's native window when the player changes
    // top-level windows (pop out / pop in), so the old HWND VLC was drawing into
    // is gone — hand VLC the new one. Playback keeps running across the swap.
    if (engine && videoWidget) engine->setOutputWindow((void*)videoWidget->winId());
}

void VideoPlayer::reownOverlays() {
    // The overlays are Qt::Tool (owned) windows: Windows keeps each one above the
    // HWND that owned it, and showing/raising an owned window drags that owner to
    // the front. Crucially, Windows binds that owner at native-window CREATION and
    // won't change it on a live window (setTransientParent is a no-op there). So
    // after the player moves between the shell and the pop-out window, the owner
    // is stale — showing an overlay yanks the WRONG window forward and the overlay
    // bleeds over windows in front of the current one.
    //
    // Fix: force each overlay's native window to be recreated against the player's
    // current top-level. Round-tripping the parent (nullptr -> this) recreates the
    // window, and Qt resolves the new owner from this->window() — the pop-out when
    // floating, the shell when docked.
    for (QWidget* w : { static_cast<QWidget*>(playerControls),
                        static_cast<QWidget*>(titleBar),
                        static_cast<QWidget*>(posterOverlay),
                        static_cast<QWidget*>(splitterToggleBtn),
                        static_cast<QWidget*>(prevPhotoBtn),
                        static_cast<QWidget*>(nextPhotoBtn),
                        static_cast<QWidget*>(visualizer) }) {
        if (!w) continue;
        const bool vis = w->isVisible();
        const Qt::WindowFlags flags = w->windowFlags();
        w->setParent(nullptr, flags); // detach: drops the stale owner
        w->setParent(this, flags);    // re-own to this->window() (pop-out or shell)
        if (vis) w->show();
    }
    repositionOverlays();
}

void VideoPlayer::hardStop() {
    // Full teardown — releases the media and emits closed() (the shell re-docks the
    // player and hides the video area). Used when the pop-out window is closed.
    closePlayer();
}

void VideoPlayer::onMediaEndReached() {
    stopRecordingIfActive(); // the live source ended — finalise any recording

    // Shorts behave like the feed at end-of-clip, honouring autoplay:
    //   • autoplay OFF -> loop this short in place (no ended chrome, no poster).
    //   • autoplay ON  -> advance to the next short via mediaEnded (the orchestrator
    //     calls playAdjacentResult(1)); seamless, the next video replaces the frame
    //     with no poster flash. Emit WITHOUT the ended-mode/poster chrome below.
    if (m_shortsMode && engine->hasMedia()) {
        if (m_autoplayEnabled) {
            emit mediaEnded();
        } else {
            engine->reloadLastMedia();
            QTimer::singleShot(50, this, [this]() {
                engine->play();
                if (playerControls) { playerControls->resetUiState(); playerControls->startPolling(); }
                });
        }
        return;
    }

    // The track ended: if the visualizer is up and the setting allows, send the
    // gulls into their dramatic spin-and-fall (otherwise they keep flying).
    if (m_visualizerActive && visualizer && m_killGullsOnEnd) visualizer->triggerDeath();

    osdTimer->stop();
    m_stopped = true; // ended = replay-ready, same as a first-stage Stop
    emit smtcStateChanged(0); // SMTC: Stopped (auto-advance re-sets Playing if a next item runs)
    // Freeze the seeker/timestamp at the end so they don't snap back to 0 while
    // VLC drains the decoder. startPolling() clears this when the next item plays.
    if (playerControls) playerControls->setEndedMode(true);
    pinOverlayWindow(playerControls, controlsFade);
    pinOverlayWindow(titleBar, titleFade);

    // Cover the final (often black) frame with the poster, controls on top.
    showPosterOverlay();
    raiseOverlays();

    repositionOverlays();

    // If a next item exists, the auto-advance replaces the poster/replay; on the
    // last item nothing advances, so the poster + replay button persist.
    emit mediaEnded();
}

void VideoPlayer::playLocalFile(const QUrl& url) {
    setMediaKind(kindForLocalFile(url));
    if (m_kind == MediaKind::Photo) { openPhoto(url); return; } // still image — no VLC

    stopRecordingIfActive(); // switching media — finalise any recording first
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear();
    m_currentLocalUrl = url; // Record clips the watched range straight from this file
    m_isLive = false;
    m_isStreaming = false; // local file — playback errors are genuine, no refetch
    m_fetching = false;    // local files load instantly, no placeholder phase
    m_stopped = false;
    m_shortsMode = false;  // new media — the orchestrator re-enables for a short
    applyNormalizationForCurrentKind(); // peak protection state, before tap start + load
    engine->setAudioTap(m_kind != MediaKind::Photo); // tap audio + video so both run our EQ/limiter
    applyEqualizerForCurrentKind(); // this kind's saved EQ, before the media loads
    emit playbackStarted(); // host shows + sizes the video area

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);

    // New media — drop the old poster; the orchestrator's thumbnailer answers
    // with this file's frame grab / cover art for the stop/EOF poster.
    m_posterPixmap = QPixmap();
    hidePosterOverlay();
    // Audio: stand a placeholder up immediately so the surface isn't black; the
    // real cover art replaces it when the thumbnailer's grab lands.
    if (m_kind == MediaKind::Audio) showAudioArt();

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    // Request with the forward-slash form the Library uses: the thumbnail disk
    // cache is keyed on the raw path STRING, so the native (backslash) form
    // would never hit the Library's cached entry and re-run ffmpeg every time.
    emit localPosterRequested(url.toLocalFile());
    engine->loadLocalFile(nativePath);

    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) playerControls->startPolling();
        });

    if (titleBar) {
        titleBar->setTitle(QFileInfo(nativePath).completeBaseName());
        titleBar->setLoading(false);
    }
    // SMTC: local files have no uploader; flag music vs video for the right widget.
    emit smtcMetadata(QFileInfo(nativePath).completeBaseName(), QString(),
                      m_kind == MediaKind::Video);
    // Local file: nothing to share, no online description.
    emit shareAvailableChanged(false);
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());
    if (playerControls) {
        playerControls->setStreamingMode(false);
        playerControls->setLiveMode(false);
        playerControls->setRecordAvailable(true); // local files can be recorded too
    }
    applyKindChrome(); // audio swaps fullscreen for the visualizer button
    showOSD();
}

void VideoPlayer::playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
    stopRecordingIfActive(); // switching media — finalise any recording first
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear(); m_currentLocalUrl.clear();

    // SoundCloud is audio-only — show the artwork poster instead of a black frame.
    setMediaKind(rawUrl.host().contains(QStringLiteral("soundcloud.com"), Qt::CaseInsensitive)
                 ? MediaKind::Audio : MediaKind::Video);
    applyNormalizationForCurrentKind(); // peak protection state, before tap start + load
    engine->setAudioTap(m_kind != MediaKind::Photo); // tap audio + video so both run our EQ/limiter
    applyEqualizerForCurrentKind(); // this kind's saved EQ, before the media loads

    // New media — clear the old poster; the probe/resolve brings a fresh thumbnail.
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    if (playerControls) {
        playerControls->resetUiState();
        playerControls->setCurrentFormat("");
    }
    lastRequestedFormatId.clear(); // new video — honour the default quality setting

    m_isStreaming = true;   // online stream — a stale cached URL can be refetched
    m_isLive = false;       // assume VOD until the probe reports live (picks record method)
    m_streamRetried = false;
    m_fetching = true;      // poster stands in for the video until playback starts
    m_stopped = false;
    m_shortsMode = false;   // new media — the orchestrator re-enables for a short

    // Reset the Info panel; the title is known now, the rest arrives with the probe.
    m_infoTitle = title;
    m_infoUploader.clear(); m_infoViews.clear(); m_infoDate.clear(); m_infoDescription.clear();
    m_commentsRequestedUrl.clear(); // new media — let the probe re-trigger a comments fetch

    emit playbackStarted();

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);
    if (m_kind == MediaKind::Audio) showAudioArt(); // placeholder until the artwork resolves

    savedStreamTimestamp = -1;
    if (titleBar) {
        if (!title.isEmpty()) titleBar->setTitle(title);
        else titleBar->setTitle((cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) ? "Starting stream..." : "Probing stream...");
        titleBar->setLoading(true); // seagull spins until playing
    }
    emit shareAvailableChanged(true); // online stream: the page URL is shareable
    // Clear the previous video's description until this one's probe reports in.
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());
    // SMTC: show the title we have now; onVideoInfo fills the uploader once probed.
    emit smtcMetadata(title.isEmpty() ? QStringLiteral("Seagull") : title, QString(),
                      m_kind == MediaKind::Video);

    if (playerControls) {
        playerControls->setStreamingMode(true);
        playerControls->setLiveMode(false); // assume VOD until the probe says otherwise
        playerControls->setRecordAvailable(true);
    }
    applyKindChrome(); // audio swaps fullscreen for the visualizer button

    currentBaseUrl = rawUrl;
    currentVideoTitle = title;

    if (cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) {
        // CDN already resolved (Queue prefetch): probe only fills the quality menu /
        // thumbnail / info, and we start the provided stream straight away.
        emit probeQualitiesRequested(rawUrl.toString());
        onStreamUrlReady(cdnVideoUrl, cdnAudioUrl);
    }
    else {
        // No prefetched CDN (e.g. a Search result): resolve + start the stream. The
        // metadata job also emits the quality menu / thumbnail / info, so we skip the
        // separate probe (it would contend with this resolve on the same worker).
        emit streamUrlRequested(rawUrl.toString(), lastRequestedFormatId, false);
    }
    showOSD();
}

void VideoPlayer::handleStopRequest() {
    // Two-stage Stop. First press: halt playback and poster the video — the
    // player stays up with the media loaded, replay-ready. Second press (or a
    // stop with nothing loaded): tear the player down and release the media.
    if (!m_stopped && engine->hasMedia()) {
        m_stopped = true;
        stopRecordingIfActive();
        retryTimer->stop();
        osdTimer->stop();
        m_fetching = false;
        engine->stop(); // media stays loaded; releaseMedia() is press #2
        emit smtcStateChanged(0); // SMTC: Stopped (media still loaded, replay-ready)
        if (playerControls) { playerControls->stopPolling(); playerControls->setEndedMode(true); }
        pinOverlayWindow(playerControls, controlsFade);
        pinOverlayWindow(titleBar, titleFade);
        // Local file whose poster grab hasn't landed (or got lost): ask again
        // now. A disk-cache hit answers synchronously, so the poster goes up
        // in this same pass; slower grabs show late via the m_stopped check.
        if (!m_isStreaming && m_posterPixmap.isNull()
            && m_currentLocalUrl.isValid() && !m_currentLocalUrl.isEmpty())
            emit localPosterRequested(m_currentLocalUrl.toLocalFile());
        showPosterOverlay();
        raiseOverlays();
        repositionOverlays();
        return;
    }
    closePlayer();
}

void VideoPlayer::onThumbnailResolved(const QString& thumbUrl) {
    if (thumbUrl.isEmpty()) return;
    QNetworkRequest req((QUrl(thumbUrl)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    // Hotlink-protected CDNs (phncdn etc.) want the page URL as Referer.
    if (!currentBaseUrl.isEmpty())
        req.setRawHeader("Referer", currentBaseUrl.toString().toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_thumbNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QByteArray data = reply->readAll();
        QPixmap pm;
        if (pm.loadFromData(data)) { applyPosterPixmap(pm); return; }
        // QPixmap couldn't decode (WebP sites, without the Qt imageformats
        // plugin) — round-trip the bytes through ffmpeg.
        SgThumbnailer::decodeViaFfmpeg(data, this, [this](const QPixmap& dec) {
            if (!dec.isNull()) applyPosterPixmap(dec);
            });
        });
}

void VideoPlayer::applyPosterPixmap(const QPixmap& pm) {
    m_posterPixmap = pm;
    if (!pm.isNull()) emit smtcArtwork(pm.toImage()); // SMTC widget cover art
    // Audio: the artwork is the surface — show it (replacing the placeholder),
    // unless the visualizer is currently showing instead.
    if (m_kind == MediaKind::Audio) {
        if (!m_visualizerActive) { showPosterOverlay(); raiseOverlays(); }
        return;
    }
    // Fetch placeholder: stand in for the (black) video until it starts; if
    // the poster is already up (EOF replay), paint the pixmap in now. Shorts skip
    // this so scrolling cuts straight to the next video with no thumbnail flash
    // (the pixmap is still kept above for SMTC artwork).
    if (m_fetching && !m_shortsMode) showPosterOverlay();
    else if (posterOverlay && posterOverlay->isVisible())
        repositionOverlays();
}

void VideoPlayer::onLocalPosterReady(const QString& filePath, const QPixmap& pixmap) {
    if (pixmap.isNull()) return;
    // Stale answers (a previous file, or we've moved on to a stream) don't apply.
    if (m_isStreaming || !m_currentLocalUrl.isValid() || m_currentLocalUrl.isEmpty()) return;
    if (QString::compare(QDir::toNativeSeparators(m_currentLocalUrl.toLocalFile()),
                         QDir::toNativeSeparators(filePath), Qt::CaseInsensitive) != 0) return;
    m_posterPixmap = pixmap;
    emit smtcArtwork(pixmap.toImage()); // SMTC widget cover art (local frame grab / cover)
    // Audio: the cover art is the surface — show it (replacing the placeholder),
    // unless the visualizer is currently showing instead.
    if (m_kind == MediaKind::Audio) {
        if (!m_visualizerActive) { showPosterOverlay(); raiseOverlays(); }
        return;
    }
    // Stopped/ended before the grab finished — poster it in now.
    if (m_stopped) {
        showPosterOverlay();
        raiseOverlays();
    }
}

void VideoPlayer::showPosterOverlay() {
    if (!posterOverlay || m_posterPixmap.isNull()) return;
    if (!isVisible()) return;
    if (m_visualizerActive) return; // the visualizer owns the surface
    posterOverlay->show();
    repositionOverlays();              // sizes + paints the scaled pixmap
    raiseOverlays();
}

void VideoPlayer::hidePosterOverlay() {
    if (posterOverlay) posterOverlay->hide();
}

void VideoPlayer::onPlaybackError() {
    // A cached stream URL can go stale (non-YouTube tokens we can't pre-validate),
    // which VLC reports as an open error. Re-resolve the link fresh and try once
    // more before giving up.
    if (m_isStreaming && !m_streamRetried) {
        m_streamRetried = true;
        m_fetching = true; // poster stands in again while the fresh URL resolves
        if (!m_posterPixmap.isNull() && !m_shortsMode) showPosterOverlay(); // shorts stay posterless
        if (titleBar) {
            titleBar->setTitle("Stream link expired — refetching...");
            titleBar->setLoading(true);
            pinOverlayWindow(titleBar, titleFade);
            if (videoAreaExposed()) titleBar->raise();
        }
        repositionOverlays();
        // The cached URL is what went stale — force a fresh yt-dlp resolve.
        emit streamUrlRequested(currentBaseUrl.toString(), lastRequestedFormatId, true);
        retryTimer->start(15000); // give the refetch+open a window, else showStreamFailed()
        return;
    }

    showStreamFailed();
}

void VideoPlayer::showStreamFailed() {
    // VLC couldn't open/play the stream (or the refetch never produced one). Tell
    // the user and offer a retry: drop into ended mode so the play button becomes a
    // replay button (re-uses the last media), and keep the controls/title pinned.
    retryTimer->stop();
    osdTimer->stop();
    m_fetching = false;
    m_stopped = true; // replay-ready; a Stop press now tears down
    if (titleBar) { titleBar->setTitle("Stream failed to load — press replay to retry."); titleBar->setLoading(false); pinOverlayWindow(titleBar, titleFade); }
    if (playerControls) { playerControls->setEndedMode(true); pinOverlayWindow(playerControls, controlsFade); }
    raiseOverlays();
    repositionOverlays();
}

void VideoPlayer::handleReplay() {
    if (!engine->hasMedia()) return;
    if (playerControls) playerControls->setEndedMode(false);
    if (m_kind == MediaKind::Audio) showAudioArt(); // keep the art; video drops the poster
    else hidePosterOverlay();
    engine->reloadLastMedia();
    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) { playerControls->resetUiState(); playerControls->startPolling(); }
        });
    showOSD();
}

void VideoPlayer::closePlayer() {
    stopRecordingIfActive(); // finalise any recording before tearing down playback
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear(); m_currentLocalUrl.clear();
    if (playerControls) playerControls->setRecordAvailable(false);
    emit shareAvailableChanged(false); // nothing playing — retire Share + Description
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());
    m_shortsMode = false;
    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();
    if (playerControls) { playerControls->stopPolling(); playerControls->setEndedMode(false); }
    m_fetching = false;
    m_stopped = false;
    engine->releaseMedia(); // stop AND unload — space bar must not resurrect it
    hidePosterOverlay();
    if (controlsFade) controlsFade->stop();
    if (titleFade) titleFade->stop();
    playerControls->hide();
    playerControls->setWindowOpacity(1.0);
    titleBar->hide();
    titleBar->setWindowOpacity(1.0);
    videoWidget->unsetCursor(); // in case we stopped while fullscreen-hidden
    if (splitterBtnHideTimer) splitterBtnHideTimer->stop();
    if (splitterBtnFade) splitterBtnFade->stop();
    if (splitterToggleBtn) splitterToggleBtn->hide();
    if (prevPhotoFade) prevPhotoFade->stop();
    if (nextPhotoFade) nextPhotoFade->stop();
    if (prevPhotoBtn) { prevPhotoBtn->hide(); prevPhotoBtn->setWindowOpacity(1.0); }
    if (nextPhotoBtn) { nextPhotoBtn->hide(); nextPhotoBtn->setWindowOpacity(1.0); }
    m_visualizerActive = false;
    if (visualizer) visualizer->hide();
    emit closed(); // host hides the video area + leaves fullscreen
}

void VideoPlayer::onSingleClickTimeout() {
    togglePlayPause();
}

void VideoPlayer::togglePlayPause() {
    // Nothing playing or player closed: a stray space bar must stay a no-op.
    if (!isVisible() || !engine->hasMedia()) return;
    // Stopped/ended: play = a clean replay from the top (resets the ended-mode
    // controls and restarts polling — a bare engine->play() would leave the
    // seeker frozen and the poster up).
    if (m_stopped) { handleReplay(); return; }
    if (engine->isPlaying()) engine->pause();
    else engine->play();
}

bool VideoPlayer::hasActiveMedia() const {
    return engine && engine->hasMedia();
}

qint64 VideoPlayer::mediaPosition() const { return engine ? engine->time() : 0; }
qint64 VideoPlayer::mediaDuration() const { return engine ? engine->length() : 0; }

void VideoPlayer::seekRelative(qint64 deltaMs) {
    if (!engine || !engine->hasMedia()) return;
    const qint64 len = engine->length();
    qint64 t = engine->time() + deltaMs;
    if (t < 0) t = 0;
    if (len > 0 && t > len) t = len;
    engine->setTime(t);
    showOSD(); // surface the seeker/controls so the jump is visible
}

void VideoPlayer::stepFrame(int dir) {
    if (!engine || !engine->hasMedia()) return;
    if (engine->state() != PlaybackEngine::State::Paused) return; // paused-only, per request
    engine->stepFrame(dir);
    showOSD();
}

void VideoPlayer::changeVolume(int delta) {
    if (playerControls) playerControls->nudgeVolume(delta);
    showOSD(); // surface the controls so the volume readout is visible
}

void VideoPlayer::toggleMute() {
    if (playerControls) playerControls->toggleMute();
    showOSD();
}

void VideoPlayer::applyEqualizer(const QVector<float>& gains, float preampDb) {
    if (engine) engine->setEqualizer(gains, preampDb);
}

void VideoPlayer::disableEqualizer() {
    if (engine) engine->disableEqualizer();
}


void VideoPlayer::setNormalizationEnabled(bool on) {
    // Live edit from the EQ tab, gated by the orchestrator to the matching kind. Audio
    // applies instantly on the sink's limiter; video reloads a local clip in place
    // (streams take it on next load).
    if (!engine) return;
    // Both audio and video now run through the tap (our SgEq + limiter), so normalization
    // is the same live sink-limiter toggle for both — no media reload needed for video.
    if (m_kind != MediaKind::Photo)
        engine->setAudioNormalizationEnabled(on);
}

void VideoPlayer::applyNormalizationForCurrentKind() {
    // Apply the saved per-kind normalization state before the media loads. The EQ tab
    // owns the config; we read + push it so a freshly started track gets its kind's
    // peak protection even if the EQ tab was never opened. Must run before setAudioTap
    // (the sink reads the state at start). Default on: "audio never peaks" out of the box.
    // Both audio and video go through the tap now, so both apply it on the sink limiter.
    if (!engine) return;
    if (m_kind == MediaKind::Photo) return; // no audio
    const QString ns = (m_kind == MediaKind::Audio) ? QStringLiteral("Eq/Audio/")
                                                    : QStringLiteral("Eq/Video/");
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    const bool on = cfg.value(ns + "NormEnabled", true).toBool();
    engine->setAudioNormalizationEnabled(on);
    // Tapping a video's audio adds the sink's buffering latency, so video would lead the
    // sound; trim it back with a negative audio delay (audio-only has nothing to sync to,
    // so 0). Stored now; the engine applies it once the input is playing. Tune by ear.
    constexpr int kVideoTapAvSyncMs = 0; // set negative (e.g. -200) if video leads audio
    engine->setTapAudioDelayMs(m_kind == MediaKind::Video ? kVideoTapAvSyncMs : 0);
}

void VideoPlayer::applyEqualizerForCurrentKind() {
    // Apply the saved EQ for whatever's playing. The EQ tab owns the config; we just
    // read + push it so a freshly started track reflects its kind's curve even if the
    // EQ tab was never opened. Photo / disabled / a band-count mismatch => no EQ.
    if (!engine) return;
    if (m_kind == MediaKind::Photo) { engine->disableEqualizer(); return; }
    const QString ns = (m_kind == MediaKind::Audio) ? QStringLiteral("Eq/Audio/")
                                                    : QStringLiteral("Eq/Video/");
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    if (!cfg.value(ns + "Enabled", false).toBool()) { engine->disableEqualizer(); return; }
    const float preamp = cfg.value(ns + "Preamp", 0.0).toFloat();
    QVector<float> gains;
    const QStringList parts = cfg.value(ns + "Gains").toString().split(',', Qt::SkipEmptyParts);
    for (const QString& p : parts) gains << p.toFloat();
    if (gains.size() == PlaybackEngine::equalizerBandCount())
        engine->setEqualizer(gains, preamp);
    else
        engine->disableEqualizer();
}

bool VideoPlayer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == videoWidget) {
        if (event->type() == QEvent::MouseButtonPress) clickTimer->start(250);
        else if (event->type() == QEvent::MouseButtonDblClick) {
            clickTimer->stop();
            emit fullscreenToggleRequested();
        }
        else if (event->type() == QEvent::Wheel && m_shortsMode) {
            const int dy = static_cast<QWheelEvent*>(event)->angleDelta().y();
            // A direction flip drops the old accumulation so opposing half-
            // notches don't cancel each other out.
            if ((dy > 0) != (m_shortsWheelAccum > 0)) m_shortsWheelAccum = 0;
            m_shortsWheelAccum += dy;
            if (qAbs(m_shortsWheelAccum) >= 120) { // one full wheel notch
                m_shortsWheelAccum = 0;
                if (m_shortsScrollClock.elapsed() > 350) { // one short per flick
                    m_shortsScrollClock.restart();
                    emit shortsScrolled(dy < 0 ? 1 : -1); // scroll down = next
                }
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoPlayer::repositionOverlays() {
    if (!videoWidget || !videoWidget->isVisible()) return;
    QPoint globalPos = videoWidget->mapToGlobal(QPoint(0, 0));
    if (posterOverlay && posterOverlay->isVisible()) {
        posterOverlay->setGeometry(globalPos.x(), globalPos.y(), videoWidget->width(), videoWidget->height());
        if (!m_posterPixmap.isNull())
            posterOverlay->setPixmap(m_posterPixmap.scaled(videoWidget->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (visualizer && visualizer->isVisible())
        visualizer->setGeometry(globalPos.x(), globalPos.y(), videoWidget->width(), videoWidget->height());
    if (titleBar) { titleBar->setFixedWidth(videoWidget->width()); titleBar->move(globalPos.x(), globalPos.y() + 5); }
    if (playerControls && playerControls->isVisible()) {
        applySeekBarWidth(); // re-clamp the bar to the current video width — never overhang
        int controlX = globalPos.x() + (videoWidget->width() - playerControls->width()) / 2;
        int controlY = globalPos.y() + videoWidget->height() - playerControls->height() - 5;
        // The chevron slides in underneath; nudge the controls up to clear it.
        if (splitterToggleBtn && splitterToggleBtn->isVisible())
            controlY -= splitterToggleBtn->height() + 4;
        playerControls->move(controlX, controlY);
    }
    if (splitterToggleBtn && splitterToggleBtn->isVisible()) positionSplitterToggle();
    if (m_kind == MediaKind::Photo) {
        const int midY = globalPos.y() + (videoWidget->height() - 44) / 2;
        if (prevPhotoBtn && prevPhotoBtn->isVisible())
            prevPhotoBtn->move(globalPos.x() + 12, midY);
        if (nextPhotoBtn && nextPhotoBtn->isVisible())
            nextPhotoBtn->move(globalPos.x() + videoWidget->width() - 44 - 12, midY);
    }
}

// The overlays are top-level tool windows, so show()/raise() stacks them above
// EVERYTHING in the app's window band — dialogs, torn-off tabs — and can even
// pop them over other applications' windows covering the video area. Before
// re-stacking, verify the top-level window at the given point is actually the
// player's surface: our own window or one of the overlays themselves. A
// covering Seagull window returns itself; a foreign app's window returns null.
bool VideoPlayer::overlaySurfaceExposedAt(const QPoint& globalPos) const {
    const QWindow* top = QGuiApplication::topLevelAt(globalPos);
    if (!top) return false;
    if (window()->windowHandle() == top) return true;
    for (const QWidget* w : { static_cast<const QWidget*>(playerControls),
                              static_cast<const QWidget*>(titleBar),
                              static_cast<const QWidget*>(posterOverlay),
                              static_cast<const QWidget*>(splitterToggleBtn),
                              static_cast<const QWidget*>(prevPhotoBtn),
                              static_cast<const QWidget*>(nextPhotoBtn),
                              static_cast<const QWidget*>(visualizer) })
        if (w && w->windowHandle() == top) return true;
    return false;
}

bool VideoPlayer::videoAreaExposed() const {
    if (!videoWidget || !videoWidget->isVisible()) return false;
    return overlaySurfaceExposedAt(videoWidget->mapToGlobal(
        QPoint(videoWidget->width() / 2, videoWidget->height() / 2)));
}

void VideoPlayer::raiseOverlays() {
    if (!videoAreaExposed()) return; // something covers the video — don't pop over it
    if (playerControls) playerControls->raise();
    if (titleBar) titleBar->raise();
}

// Called just before enterFullScreen / exitFullScreen triggers the OS window
// state change. Hides both overlay windows so they don't jump or trail while
// DWM animates the frame; showOverlaysAfterTransition() restores them once the
// window has settled.
void VideoPlayer::suppressOverlaysForTransition() {
    if (playerControls && playerControls->isVisible()) playerControls->hide();
    if (titleBar && titleBar->isVisible()) titleBar->hide();
}

// Called from the singleShot timer in enterFullScreen / exitFullScreen after
// the window animation has finished. Re-shows the overlays, repositions them
// to the now-settled frame geometry, and re-stacks them above the VLC surface.
void VideoPlayer::showOverlaysAfterTransition() {
    if (playerControls) playerControls->show();
    if (titleBar) titleBar->show();
    repositionOverlays();
    raiseOverlays();
}

void VideoPlayer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionOverlays();
}

void VideoPlayer::showOSD() {
    videoWidget->unsetCursor(); // fullscreen idle-hide: movement brings the cursor back
    if (m_kind == MediaKind::Photo) {
        // Photos have no transport bar — just the two side arrows.
        fadeOverlayWindow(prevPhotoBtn, prevPhotoFade, true);
        fadeOverlayWindow(nextPhotoBtn, nextPhotoFade, true);
        repositionOverlays();
        if (prevPhotoBtn) prevPhotoBtn->raise();
        if (nextPhotoBtn) nextPhotoBtn->raise();
        osdTimer->start(3000);
        return;
    }
    fadeOverlayWindow(playerControls, controlsFade, true);
    fadeOverlayWindow(titleBar, titleFade, true);
    // Only re-stack above the poster when it's actually showing (paused / EOF,
    // or audio album art). Skip while a control popup is open — raising the
    // overlays would bury the volume/quality popup (it's a separate top-level).
    if (posterOverlay && posterOverlay->isVisible()
        && !(playerControls && playerControls->hasOpenPopup()))
        raiseOverlays();
    repositionOverlays();
    osdTimer->start(3000);
}

void VideoPlayer::hideOSD() {
    if (m_kind == MediaKind::Photo) {
        // No popups/playback gating for a still image — just fade the arrows out.
        fadeOverlayWindow(prevPhotoBtn, prevPhotoFade, false);
        fadeOverlayWindow(nextPhotoBtn, nextPhotoFade, false);
        return;
    }
    // The volume/quality popups always close BEFORE the controls go. If the
    // cursor is actually on one (mid-interaction), keep everything and re-arm;
    // otherwise shut the popups now and let the bars fade out after them.
    if (playerControls && playerControls->hasOpenPopup()) {
        if (playerControls->popupUnderCursor()) {
            osdTimer->start(3000);
            return;
        }
        playerControls->closePopups();
    }
    if (engine->isPlaying()) {
        fadeOverlayWindow(playerControls, controlsFade, false);
        // Keep the title bar up while a clip is saving (seagull + "Saving clip…")
        // and while a brief save-confirmation notice is showing.
        if (titleBar && !m_clipBusy && !m_bannerNotice)
            fadeOverlayWindow(titleBar, titleFade, false);
        // Fullscreen: the cursor goes with the chrome (YouTube-style); any
        // mouse movement brings both back via showOSD.
        if (window()->isFullScreen())
            videoWidget->setCursor(Qt::BlankCursor);
    }
}

void VideoPlayer::checkMouseMovement() {
    if (!isVisible()) return;
    QPoint currentPos = QCursor::pos();
    if (currentPos != lastMousePos) {
        lastMousePos = currentPos;
        QRect videoRect(videoWidget->mapToGlobal(QPoint(0, 0)), videoWidget->size());
        // The geometric hit isn't enough: the cursor may be over a dialog, a
        // torn-off tab, or another app's window covering the video area — the
        // OSD popping up would draw the overlays above that window.
        if (videoRect.contains(currentPos) && overlaySurfaceExposedAt(currentPos))
            showOSD();
    }
    // Re-evaluated every tick (not just on movement): the zone itself moves
    // under a stationary cursor — toggling resizes the video, the controls
    // pill comes and goes with the OSD.
    updateSplitterToggle(currentPos);
}

void VideoPlayer::updateSplitterToggle(const QPoint& globalPos) {
    if (!splitterToggleBtn || m_poppedOut) return; // no tabs pane to toggle while floating
    bool inTrigger = false;
    if (videoWidget && videoWidget->isVisible() && overlaySurfaceExposedAt(globalPos)) {
        const QPoint tl = videoWidget->mapToGlobal(QPoint(0, 0));
        const QRect videoRect(tl, videoWidget->size());
        // "Hovering the splitter": only the centre of the seam, where a
        // splitter's grip dots sit — a small band spanning the bottom sliver
        // of the video plus the handle just below it. The button's own rect
        // counts too so hovering it doesn't start the hide.
        const int zoneW = 120;
        const QRect zone(videoRect.left() + (videoRect.width() - zoneW) / 2,
                         videoRect.bottom() - 10, zoneW, 10 + 8);
        const bool overControls = playerControls && playerControls->isVisible()
                               && playerControls->geometry().contains(globalPos);
        inTrigger = !overControls
                 && (zone.contains(globalPos)
                     || (splitterToggleBtn->isVisible()
                         && splitterToggleBtn->geometry().contains(globalPos)));
    }

    if (inTrigger) {
        splitterBtnHideTimer->stop();
        const bool appearing = !splitterToggleBtn->isVisible();
        if (appearing) positionSplitterToggle(); // place it before it becomes visible
        fadeSplitterToggle(true);
        // Appearing nudges the controls up to clear the chevron (YouTube-style),
        // so run the full pass; otherwise just keep the button glued.
        if (appearing) repositionOverlays();
        else positionSplitterToggle();
    }
    else if (splitterToggleBtn->isVisible()) {
        // While the cursor is parked on the lifted controls, keep the chevron:
        // hiding it would drop the pill mid-interaction (e.g. during a seek).
        const bool onControls = playerControls && playerControls->isVisible()
                             && playerControls->geometry().contains(globalPos);
        if (onControls) splitterBtnHideTimer->stop();
        else if (!splitterBtnHideTimer->isActive()) splitterBtnHideTimer->start();
    }
}

// Shared fade for the top-level overlay windows; reversible mid-flight (a
// fade-in picks up from wherever a running fade-out left the opacity).
void VideoPlayer::fadeOverlayWindow(QWidget* w, QPropertyAnimation* anim, bool in) {
    if (!w || !anim) return;
    anim->stop();
    if (in) {
        if (!w->isVisible()) { w->setWindowOpacity(0.0); w->show(); }
        if (w->windowOpacity() >= 1.0) return; // already settled
    }
    else if (!w->isVisible()) return;
    anim->setDuration(in ? kOverlayFadeInMs : kOverlayFadeOutMs);
    anim->setStartValue(w->windowOpacity());
    anim->setEndValue(in ? 1.0 : 0.0);
    anim->start();
}

// Pinned show: instant and fully opaque, cancelling any in-flight fade-out
// (otherwise the fade's finished handler would hide the freshly shown window).
void VideoPlayer::pinOverlayWindow(QWidget* w, QPropertyAnimation* anim) {
    if (!w) return;
    if (anim) anim->stop();
    w->setWindowOpacity(1.0);
    w->show();
}

void VideoPlayer::fadeSplitterToggle(bool in) {
    if (in && !splitterToggleBtn->isVisible()) {
        // fadeOverlayWindow shows it at opacity 0; raise before the first frame.
        fadeOverlayWindow(splitterToggleBtn, splitterBtnFade, true);
        splitterToggleBtn->raise();
        return;
    }
    if (in) splitterToggleBtn->raise();
    fadeOverlayWindow(splitterToggleBtn, splitterBtnFade, in);
}

void VideoPlayer::positionSplitterToggle() {
    if (!splitterToggleBtn || !videoWidget) return;
    // The very bottom centre, underneath the controls (they lift to clear it).
    const QPoint tl = videoWidget->mapToGlobal(QPoint(0, 0));
    const int x = tl.x() + (videoWidget->width() - splitterToggleBtn->width()) / 2;
    const int y = tl.y() + videoWidget->height() - splitterToggleBtn->height() - 4;
    splitterToggleBtn->move(x, y);
}

void VideoPlayer::handleAvailableQualities(const QList<StreamOption>& options) {
    // If the user hasn't manually picked a quality this session, highlight the
    // option matching the default Stream Quality setting so the OSD reflects reality.
    if (lastRequestedFormatId.isEmpty()) {
        QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
        QString defaultLabel = settings.value("Streaming/Quality", "Best Available").toString();

        if (!defaultLabel.isEmpty() && defaultLabel != "Best Available") {
            QRegularExpression re("(\\d+)");
            int wantHeight = re.match(defaultLabel).captured(1).toInt();

            QString bestMatchId;
            int bestMatchHeight = -1;
            for (const StreamOption& opt : options) {
                QString h = re.match(opt.label).captured(1);
                if (h.isEmpty()) continue;          // skip "Auto"
                int oh = h.toInt();
                if (oh <= wantHeight && oh > bestMatchHeight) {
                    bestMatchHeight = oh;
                    bestMatchId = opt.formatId;
                }
            }
            if (!bestMatchId.isEmpty() && playerControls)
                playerControls->setCurrentFormat(bestMatchId);
        }
    }

    playerControls->setAvailableQualities(options);
}

void VideoPlayer::changeStreamQuality(const QString& formatId) {
    lastRequestedFormatId = formatId;
    savedStreamTimestamp = engine->time();
    if (engine->isPlaying()) engine->pause();

    if (playerControls) playerControls->setCurrentFormat(formatId);

    if (titleBar) { titleBar->setTitle("Buffering new quality..."); titleBar->setLoading(true); }
    QTimer::singleShot(200, this, [this, formatId]() { emit streamUrlRequested(currentBaseUrl.toString(), formatId, false); });
}

void VideoPlayer::onStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    retryTimer->stop(); // a (re)resolved URL arrived — the refetch window is satisfied
    if (titleBar) titleBar->setTitle(currentVideoTitle.isEmpty() ? "Streaming..." : currentVideoTitle);

    // Remember the exact URLs feeding VLC so the recorder captures the same stream
    // (for Twitch this is the local ad-free proxy URL).
    m_recordVideoUrl = videoUrl;
    m_recordAudioUrl = audioUrl;

    const qint64 startMs = (savedStreamTimestamp > 0) ? savedStreamTimestamp : 0;
    // Pass the page URL as Referer so hotlink-protected CDNs accept the stream.
    engine->loadStream(videoUrl, audioUrl, startMs, currentBaseUrl.toString());
    savedStreamTimestamp = -1;

    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) {
            playerControls->startPolling();
            playerControls->setCurrentFormat(lastRequestedFormatId); // make sure the menu shows the right one
        }
        });

    QPointer<PlayerControls> safeControls = playerControls;
    QTimer::singleShot(250, this, [safeControls]() {
        if (safeControls) safeControls->applyAudioState();
        });
}

void VideoPlayer::onLiveStatus(bool isLive) {
    m_isLive = isLive; // selects live (ffmpeg) vs VOD (yt-dlp) recording
    if (playerControls) playerControls->setLiveMode(isLive);
}

void VideoPlayer::toggleRecording() {
    // Live stream: parallel ffmpeg -c copy capture of the resolved stream URL.
    if (m_isStreaming && m_isLive) {
        if (m_recording) { emit recordStopRequested(); return; }
        if (!m_recordVideoUrl.isValid() || m_recordVideoUrl.isEmpty()) return;
        const QString title = currentVideoTitle.isEmpty() ? QStringLiteral("stream") : currentVideoTitle;
        emit recordStartRequested(m_recordVideoUrl, m_recordAudioUrl, currentBaseUrl.toString(), title);
        return;
    }

    // VOD + local file: record the *watched* range. 1st press marks the start (pulse);
    // 2nd press marks the end and saves [start,end] in the background, returning the
    // button to idle.
    if (m_clipMarking) {
        const qint64 endMs = engine->time();
        m_clipMarking = false;
        if (playerControls) playerControls->setRecording(false); // stop pulsing — marking done
        const bool isLocal = !m_isStreaming;
        const QUrl clipVideo = isLocal ? m_currentLocalUrl : m_recordVideoUrl;
        const bool haveSource = isLocal ? (clipVideo.isValid() && !clipVideo.isEmpty())
                                        : !currentBaseUrl.isEmpty();
        if (haveSource && endMs > m_clipStartMs) {
            m_clipBusy = true;
            QString title = currentVideoTitle;
            if (title.isEmpty() && isLocal)
                title = QFileInfo(m_currentLocalUrl.toLocalFile()).completeBaseName();
            if (title.isEmpty()) title = QStringLiteral("clip");
            if (titleBar) {                          // seagull + "Saving clip…" until the file is ready
                titleBar->setTitle(QStringLiteral("Saving clip…"));
                titleBar->setLoading(true);
                pinOverlayWindow(titleBar, titleFade);
                if (videoAreaExposed()) titleBar->raise();
            }
            // Pause playback while a STREAM clip downloads — the player and the grab
            // pull from the same CDN and starve each other, crawling the cut to a halt.
            // A local cut reads from disk and is near-instant; keep playing.
            m_resumeAfterClip = !isLocal && engine->isPlaying();
            if (m_resumeAfterClip) engine->pause();
            // Hand over the resolved CDN URLs feeding VLC (or the local file path) so
            // the recorder cuts the section directly (no yt-dlp re-resolve).
            emit recordClipRequested(isLocal ? QString() : currentBaseUrl.toString(),
                clipVideo, isLocal ? QUrl() : m_recordAudioUrl,
                m_clipStartMs, endMs, title);
        }
        return;
    }
    if (m_clipBusy) return; // a clip is still saving — ignore presses until it's ready
    m_clipStartMs = engine->time();
    m_clipMarking = true;
    if (playerControls) playerControls->setRecording(true); // pulse while marking the range
    showOSD();
}

void VideoPlayer::stopRecordingIfActive() {
    if (m_recording) emit recordStopRequested();          // live/local ffmpeg
    if (m_clipMarking) {                                  // abandon an unfinished mark
        m_clipMarking = false;
        if (playerControls) playerControls->setRecording(false);
    }
    if (m_clipBusy) {                                     // cancel an in-flight clip save
        m_clipCancelled = true;                           // deliberate — no failure banner
        emit recordClipCancelRequested();
    }
}

void VideoPlayer::onRecordingStarted() {
    m_recording = true;
    if (playerControls) playerControls->setRecording(true);
}

void VideoPlayer::onRecordingStopped(const QString& filePath, bool ok) {
    m_recording = false;
    if (playerControls) playerControls->setRecording(false);
    if (!filePath.isEmpty())
        showBannerNotice(ok ? "Recording saved — " + QFileInfo(filePath).fileName()
                            : QStringLiteral("Recording failed"));
}

void VideoPlayer::onClipFinished(const QString& filePath, bool ok) {
    // Fires only when the clip file is fully written (or the save was cancelled on
    // teardown) — so the seagull + "Saving clip…" stay up until the file is ready.
    m_clipBusy = false;
    if (playerControls) playerControls->setRecording(false);
    if (m_clipCancelled) { // we tore it down on purpose — just put the title back
        m_clipCancelled = false;
        m_resumeAfterClip = false; // teardown/new media owns playback now
        if (titleBar) titleBar->setLoading(false);
        restoreBannerTitle();
        return;
    }
    if (m_resumeAfterClip) { // we paused for the grab — pick playback back up
        m_resumeAfterClip = false;
        engine->play();
    }
    showBannerNotice(ok && !filePath.isEmpty()
        ? "Clip saved — " + QFileInfo(filePath).fileName()
        : QStringLiteral("Clip save failed"));
}

void VideoPlayer::restoreBannerTitle() {
    if (!titleBar) return;
    QString t = currentVideoTitle;
    if (t.isEmpty() && m_currentLocalUrl.isValid() && !m_currentLocalUrl.isEmpty())
        t = QFileInfo(m_currentLocalUrl.toLocalFile()).completeBaseName();
    titleBar->setTitle(t.isEmpty() ? QStringLiteral("Streaming...") : t);
}

void VideoPlayer::showBannerNotice(const QString& text) {
    // The player may already be closed when a queued recorder result lands; a
    // banner overlay popping up over the tabs would be stray, so skip it.
    if (!titleBar || !isVisible()) return;
    m_bannerNotice = true;
    titleBar->setLoading(false); // the seagull bows out — the work is done
    titleBar->setTitle(text);
    pinOverlayWindow(titleBar, titleFade);
    if (videoAreaExposed()) titleBar->raise();
    repositionOverlays();
    QTimer::singleShot(4000, this, [this]() {
        m_bannerNotice = false;
        if (m_clipBusy) return; // a new save started — its "Saving clip…" owns the banner
        restoreBannerTitle();
        // If the OSD already timed out, the banner was only up for the notice.
        if (titleBar && engine->isPlaying() && !osdTimer->isActive())
            fadeOverlayWindow(titleBar, titleFade, false);
        });
}

void VideoPlayer::onVideoInfo(const QString& title, const QString& uploader,
    const QString& views, const QString& date, const QString& description) {
    if (!title.isEmpty()) m_infoTitle = title;
    m_infoUploader = uploader;
    m_infoViews = views;
    m_infoDate = date;
    m_infoDescription = description;
    // The shell opens/closes the Description tab off this.
    emit videoInfoChanged(m_infoTitle, uploader, views, date, description);
    // SMTC: refresh now that the uploader (artist/subtitle) is known.
    emit smtcMetadata(m_infoTitle, uploader, m_kind == MediaKind::Video);
}

void VideoPlayer::onCommentCount(int count) {
    // Announce a Comments tab for online VOD that has comments. Skip live (no useful
    // comments) and de-dup per page URL (the probe re-reports on quality switches),
    // so the shell doesn't reset an already-loaded Comments tab. The shell fetches
    // the actual comments lazily — only when the tab is viewed — so it never competes
    // with the stream.
    const QString pageUrl = currentBaseUrl.toString();
    if (!m_isStreaming || m_isLive || pageUrl.isEmpty() || pageUrl == m_commentsRequestedUrl) return;
    m_commentsRequestedUrl = pageUrl;
    emit commentsAvailable(pageUrl, count);
}

void VideoPlayer::shareLink() {
    if (currentBaseUrl.isEmpty()) return;
    QGuiApplication::clipboard()->setText(currentBaseUrl.toString());
    // Brief confirmation in the banner, then restore the title.
    if (titleBar) titleBar->setTitle("Link copied to clipboard");
    QTimer::singleShot(1500, this, [this]() {
        if (titleBar) titleBar->setTitle(currentVideoTitle.isEmpty() ? "Streaming..." : currentVideoTitle);
        });
}

MediaKind VideoPlayer::kindForLocalFile(const QUrl& url) {
    const QString ext = QFileInfo(url.toLocalFile()).suffix().toLower();
    static const QStringList audioExts = { "mp3","m4a","aac","flac","opus","wav","ogg","wma","aiff","alac" };
    static const QStringList imageExts = { "jpg","jpeg","png","gif","bmp","webp","tif","tiff","heic","heif" };
    if (audioExts.contains(ext)) return MediaKind::Audio;
    if (imageExts.contains(ext)) return MediaKind::Photo;
    return MediaKind::Video;
}

void VideoPlayer::setMediaKind(MediaKind k) {
    if (m_kind == k) return;
    m_kind = k;
    emit mediaKindChanged(k); // lets the EQ follow the playing kind while its page is open
}

void VideoPlayer::applyKindChrome() {
    // Audio has no fullscreen video surface, so its controls show a visualizer
    // button where the fullscreen button sits.
    if (playerControls) playerControls->setVisualizerMode(m_kind == MediaKind::Audio);
    // Leaving photo mode (to video/audio): retire the side arrows.
    if (m_kind != MediaKind::Photo) {
        if (prevPhotoFade) prevPhotoFade->stop();
        if (nextPhotoFade) nextPhotoFade->stop();
        if (prevPhotoBtn) { prevPhotoBtn->hide(); prevPhotoBtn->setWindowOpacity(1.0); }
        if (nextPhotoBtn) { nextPhotoBtn->hide(); nextPhotoBtn->setWindowOpacity(1.0); }
    }
    if (m_kind == MediaKind::Audio) {
        // Restore the persisted on/off choice (survives track changes + restarts).
        // Defaults ON: the audio player leads with the seagull visualizer.
        QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
        m_visualizerActive = cfg.value("Visualizer/Active", true).toBool();
        if (playerControls) playerControls->setVisualizerActive(m_visualizerActive);
        if (m_visualizerActive && visualizer) {
            hidePosterOverlay();
            visualizer->setDemoMode(!engine->audioTapActive());
            visualizer->show();
            repositionOverlays();
            raiseOverlays();
        } else if (visualizer) {
            visualizer->hide();
        }
    } else {
        // Video / photo: no visualizer.
        m_visualizerActive = false;
        if (playerControls) playerControls->setVisualizerActive(false);
        if (visualizer) { visualizer->setDemoMode(false); visualizer->hide(); }
    }
}

QPixmap VideoPlayer::audioPlaceholder() {
    if (!m_audioPlaceholder.isNull()) return m_audioPlaceholder;
    // A centred music note on transparency; the poster's black background shows
    // through. Rendered large so scaling down onto the surface stays crisp.
    const int sz = 512;
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QSvgRenderer r(QStringLiteral(":/Assets/icons/music-note.svg"));
    QPainter p(&pm);
    const int glyph = sz / 2;
    r.render(&p, QRectF((sz - glyph) / 2.0, (sz - glyph) / 2.0, glyph, glyph));
    p.end();
    m_audioPlaceholder = pm;
    return m_audioPlaceholder;
}

void VideoPlayer::showAudioArt() {
    if (m_kind != MediaKind::Audio) return;
    if (m_visualizerActive) return; // the visualizer owns the surface right now
    // Cover art if we have it, otherwise the music-note placeholder.
    if (m_posterPixmap.isNull()) m_posterPixmap = audioPlaceholder();
    showPosterOverlay();
}

void VideoPlayer::applyVisualizerSettings() {
    if (!visualizer) return;
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    const QString type = cfg.value("Visualizer/Type", "Seagull Sky").toString();
    visualizer->setMode(type);
    // Behaviour is global — one key shared by every visualizer.
    visualizer->setBehavior(cfg.value("Visualizer/Behavior", "Drift").toString());
    visualizer->setMaxGulls(cfg.value("Visualizer/MaxGulls", 14).toInt());
    m_killGullsOnEnd = cfg.value("Visualizer/KillOnEnd", true).toBool();
}

void VideoPlayer::setVisualizerSuspended(bool on) {
    if (visualizer) visualizer->suspendRendering(on);
}

void VideoPlayer::setSeekBarSize(int width) {
    m_seekBarTargetWidth = width;
    applySeekBarWidth();   // clamp to the video frame, then re-centre
    repositionOverlays();
}

void VideoPlayer::applySeekBarWidth() {
    if (!playerControls || !videoWidget) return;
    // Keep a clear gap each side so the pill (even with the audio viz triangles out,
    // ~22px/side) never reaches the video edge. A narrow window shrinks the bar to fit;
    // a wide one lets it grow up to the chosen target.
    constexpr int kEdgeGap = 30;
    const int avail = videoWidget->width() - 2 * kEdgeGap;
    playerControls->setBaseWidth(qMin(m_seekBarTargetWidth, avail));
}

void VideoPlayer::cycleVisualizer(int delta) {
    if (!visualizer) return;
    static const QStringList kTypes = { "Seagull Sky", "Seagull Waves" };
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    const QString cur = cfg.value("Visualizer/Type", "Seagull Sky").toString();
    int idx = qMax(0, int(kTypes.indexOf(cur)));
    idx = (idx + delta + kTypes.size()) % kTypes.size();
    cfg.setValue("Visualizer/Type", kTypes[idx]);
    cfg.sync();
    applyVisualizerSettings(); // re-reads Type -> setMode + the new type's gull style (live)
}

void VideoPlayer::toggleVisualizer() {
    if (m_kind != MediaKind::Audio || !visualizer) return; // audio-only feature
    m_visualizerActive = !m_visualizerActive;
    // Persist the on/off choice so it restores next track / next launch.
    {
        QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
        cfg.setValue("Visualizer/Active", m_visualizerActive);
        cfg.sync();
    }
    if (playerControls) playerControls->setVisualizerActive(m_visualizerActive);
    if (m_visualizerActive) {
        hidePosterOverlay();           // the album art steps aside
        // Real reactivity when the audio tap is live; demo self-drive otherwise.
        visualizer->setDemoMode(!engine->audioTapActive());
        visualizer->show();
        repositionOverlays();
        raiseOverlays();               // keep the controls/title above the sky
    } else {
        visualizer->setDemoMode(false);
        visualizer->hide();
        showAudioArt();                // album art / placeholder returns
    }
}

void VideoPlayer::openPhoto(const QUrl& url) {
    stopRecordingIfActive();
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear();
    m_currentLocalUrl = url;
    setMediaKind(MediaKind::Photo);
    m_isLive = false; m_isStreaming = false; m_fetching = false; m_stopped = false;
    m_shortsMode = false;

    engine->setAudioTap(false); // back to VLC audio output (no tap for stills)
    emit playbackStarted();  // host shows + sizes the surface
    engine->releaseMedia();  // a still image: nothing for VLC to play
    mouseTrackerTimer->start(100);

    // No transport chrome for photos — drop the controls/title, leaving just the
    // two fading side arrows.
    if (controlsFade) controlsFade->stop();
    if (titleFade)    titleFade->stop();
    if (playerControls) {
        playerControls->stopPolling();
        playerControls->hide();
        playerControls->setWindowOpacity(1.0);
    }
    if (titleBar) { titleBar->hide(); titleBar->setWindowOpacity(1.0); }
    emit shareAvailableChanged(false);
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());

    // Display the image on the poster surface (KeepAspectRatio, centred on black).
    const QString path = url.toLocalFile();
    QPixmap pm(path);
    if (!pm.isNull()) {
        m_posterPixmap = pm;
        showPosterOverlay();
    } else {
        // Formats Qt can't decode without a plugin (e.g. WebP) — ffmpeg fallback,
        // same round-trip the stream poster uses.
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            SgThumbnailer::decodeViaFfmpeg(bytes, this, [this](const QPixmap& dec) {
                if (!dec.isNull() && m_kind == MediaKind::Photo) {
                    m_posterPixmap = dec;
                    showPosterOverlay();
                }
                });
        }
    }
    showOSD(); // bring the arrows up briefly
}
