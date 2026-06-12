#include "VideoPlayer.h"
#include "../Backend/PlaybackEngine.h"
#include "../Backend/SgYtDlp.h"          // StreamOption
#include "Widgets/PlayerControls.h"
#include "Widgets/PlayerTitleBar.h"

#include <QVBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QTimer>
#include <QEvent>
#include <QResizeEvent>
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
#include <QRect>
#include <QSettings>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

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

    // winId() forces the native window to exist; defer so it's ready first.
    QTimer::singleShot(0, this, [this]() {
        engine->setOutputWindow((void*)videoWidget->winId());
        });

    connect(engine, &PlaybackEngine::endReached, this, &VideoPlayer::onMediaEndReached);
    connect(engine, &PlaybackEngine::paused, this, &VideoPlayer::showPosterOverlay);
    connect(engine, &PlaybackEngine::playing, this, [this]() {
        hidePosterOverlay();
        if (titleBar) titleBar->setLoading(false); // playback started — stop the seagull
        });
    connect(engine, &PlaybackEngine::errorOccurred, this, &VideoPlayer::onPlaybackError);

    updateOverlayTimer = new QTimer(this);
    updateOverlayTimer->setSingleShot(true);
    connect(updateOverlayTimer, &QTimer::timeout, this, &VideoPlayer::repositionOverlays);

    Qt::WindowFlags overlayFlags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;

    playerControls = new PlayerControls(engine, this);
    playerControls->setWindowFlags(overlayFlags);
    playerControls->setAttribute(Qt::WA_TranslucentBackground);

    titleBar = new PlayerTitleBar(this);
    titleBar->setWindowFlags(overlayFlags);
    titleBar->setAttribute(Qt::WA_TranslucentBackground);

    // Poster overlay: opaque image window that covers the video frame. Mouse
    // events pass straight through so clicking the video still toggles playback.
    posterOverlay = new QLabel(this);
    posterOverlay->setWindowFlags(overlayFlags | Qt::WindowTransparentForInput);
    posterOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    posterOverlay->setStyleSheet("background-color: black;");
    posterOverlay->setAlignment(Qt::AlignCenter);
    posterOverlay->setScaledContents(false);
    posterOverlay->hide();

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
    videoWidget->installEventFilter(this);

    connect(playerControls, &PlayerControls::qualitySelected, this, &VideoPlayer::changeStreamQuality);
    connect(playerControls, &PlayerControls::stopRequested, this, &VideoPlayer::handleStopRequest);
    connect(playerControls, &PlayerControls::replayRequested, this, &VideoPlayer::handleReplay);
    connect(playerControls, &PlayerControls::skipRequested, this, [this](int delta) { emit skipRequested(delta); });
    connect(playerControls, &PlayerControls::fullscreenRequested, this, [this]() { emit fullscreenToggleRequested(); });
    connect(playerControls, &PlayerControls::recordToggleRequested, this, &VideoPlayer::toggleRecording);
}

void VideoPlayer::onMediaEndReached() {
    stopRecordingIfActive(); // the live source ended — finalise any recording
    osdTimer->stop();
    // Freeze the seeker/timestamp at the end so they don't snap back to 0 while
    // VLC drains the decoder. startPolling() clears this when the next item plays.
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); }
    if (titleBar) titleBar->show();

    // Cover the final (often black) frame with the poster, controls on top.
    showPosterOverlay();
    raiseOverlays();

    repositionOverlays();

    // If a next item exists, the auto-advance replaces the poster/replay; on the
    // last item nothing advances, so the poster + replay button persist.
    emit mediaEnded();
}

void VideoPlayer::playLocalFile(const QUrl& url) {
    stopRecordingIfActive(); // switching media — finalise any recording first
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear();
    m_currentLocalUrl = url; // Record clips the watched range straight from this file
    m_isLive = false;
    m_isStreaming = false; // local file — playback errors are genuine, no refetch
    emit playbackStarted(); // host shows + sizes the video area

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);

    // New media — drop any old poster (local files have no thumbnail to fetch).
    m_posterPixmap = QPixmap();
    hidePosterOverlay();

    QString nativePath = QDir::toNativeSeparators(url.toLocalFile());
    engine->loadLocalFile(nativePath);

    QTimer::singleShot(50, this, [this]() {
        engine->play();
        if (playerControls) playerControls->startPolling();
        });

    if (titleBar) {
        titleBar->setTitle(QFileInfo(nativePath).completeBaseName());
        titleBar->setLoading(false);
    }
    // Local file: nothing to share, no online description.
    emit shareAvailableChanged(false);
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());
    if (playerControls) {
        playerControls->setStreamingMode(false);
        playerControls->setLiveMode(false);
        playerControls->setRecordAvailable(true); // local files can be recorded too
    }
    showOSD();
}

void VideoPlayer::playVideo(const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
    stopRecordingIfActive(); // switching media — finalise any recording first
    m_recordVideoUrl.clear(); m_recordAudioUrl.clear(); m_currentLocalUrl.clear();

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

    // Reset the Info panel; the title is known now, the rest arrives with the probe.
    m_infoTitle = title;
    m_infoUploader.clear(); m_infoViews.clear(); m_infoDate.clear(); m_infoDescription.clear();

    emit playbackStarted();

    engine->setOutputWindow((void*)videoWidget->winId());
    mouseTrackerTimer->start(100);

    savedStreamTimestamp = -1;
    if (titleBar) {
        if (!title.isEmpty()) titleBar->setTitle(title);
        else titleBar->setTitle((cdnVideoUrl.isValid() && !cdnVideoUrl.isEmpty()) ? "Starting stream..." : "Probing stream...");
        titleBar->setLoading(true); // seagull spins until playing
    }
    emit shareAvailableChanged(true); // online stream: the page URL is shareable
    // Clear the previous video's description until this one's probe reports in.
    emit videoInfoChanged(QString(), QString(), QString(), QString(), QString());

    if (playerControls) {
        playerControls->setStreamingMode(true);
        playerControls->setLiveMode(false); // assume VOD until the probe says otherwise
        playerControls->setRecordAvailable(true);
    }

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
    closePlayer();
}

void VideoPlayer::onThumbnailResolved(const QString& thumbUrl) {
    if (thumbUrl.isEmpty()) return;
    QNetworkRequest req((QUrl(thumbUrl)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_thumbNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pm;
        if (!pm.loadFromData(reply->readAll())) return; // e.g. webp w/o plugin
        m_posterPixmap = pm;
        // If we're already sitting paused/ended, paint it in now.
        if (posterOverlay && posterOverlay->isVisible())
            repositionOverlays();
        });
}

void VideoPlayer::showPosterOverlay() {
    if (!posterOverlay || m_posterPixmap.isNull()) return;
    if (!isVisible()) return;
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
        if (titleBar) {
            titleBar->setTitle("Stream link expired — refetching...");
            titleBar->setLoading(true);
            titleBar->show();
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
    if (titleBar) { titleBar->setTitle("Stream failed to load — press replay to retry."); titleBar->setLoading(false); titleBar->show(); }
    if (playerControls) { playerControls->setEndedMode(true); playerControls->show(); }
    raiseOverlays();
    repositionOverlays();
}

void VideoPlayer::handleReplay() {
    if (!engine->hasMedia()) return;
    if (playerControls) playerControls->setEndedMode(false);
    hidePosterOverlay();
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
    mouseTrackerTimer->stop();
    osdTimer->stop();
    clickTimer->stop();
    updateOverlayTimer->stop();
    if (playerControls) { playerControls->stopPolling(); playerControls->setEndedMode(false); }
    engine->stop();
    hidePosterOverlay();
    playerControls->hide();
    titleBar->hide();
    emit closed(); // host hides the video area + leaves fullscreen
}

void VideoPlayer::onSingleClickTimeout() {
    togglePlayPause();
}

void VideoPlayer::togglePlayPause() {
    if (engine->isPlaying()) engine->pause();
    else engine->play();
}

bool VideoPlayer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == videoWidget) {
        if (event->type() == QEvent::MouseButtonPress) clickTimer->start(250);
        else if (event->type() == QEvent::MouseButtonDblClick) {
            clickTimer->stop();
            emit fullscreenToggleRequested();
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
    if (titleBar) { titleBar->setFixedWidth(videoWidget->width()); titleBar->move(globalPos.x(), globalPos.y() + 5); }
    if (playerControls && playerControls->isVisible()) {
        int controlX = globalPos.x() + (videoWidget->width() - playerControls->width()) / 2;
        int controlY = globalPos.y() + videoWidget->height() - playerControls->height() - 5;
        playerControls->move(controlX, controlY);
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
                              static_cast<const QWidget*>(posterOverlay) })
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

void VideoPlayer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionOverlays();
}

void VideoPlayer::showOSD() {
    if (playerControls) playerControls->show();
    if (titleBar) titleBar->show();
    // Only re-stack above the poster when it's actually showing (paused / EOF).
    // Raising on every mouse-move otherwise buries the volume/quality popups.
    if (posterOverlay && posterOverlay->isVisible())
        raiseOverlays();
    repositionOverlays();
    osdTimer->start(3000);
}

void VideoPlayer::hideOSD() {
    // Keep the OSD up while a volume/quality popup is open, otherwise hiding the
    // controls also tears down the popup the user is interacting with.
    if (playerControls && playerControls->hasOpenPopup()) {
        osdTimer->start(3000); // re-arm so it hides once the popup closes
        return;
    }
    if (engine->isPlaying()) {
        if (playerControls) playerControls->hide();
        // Keep the title bar up while a clip is saving (seagull + "Saving clip…")
        // and while a brief save-confirmation notice is showing.
        if (titleBar && !m_clipBusy && !m_bannerNotice) titleBar->hide();
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
}

void VideoPlayer::handleAvailableQualities(const QList<StreamOption>& options) {
    // If the user hasn't manually picked a quality this session, highlight the
    // option matching the default Stream Quality setting so the OSD reflects reality.
    if (lastRequestedFormatId.isEmpty()) {
        QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
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
                titleBar->show();
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
    titleBar->show();
    if (videoAreaExposed()) titleBar->raise();
    repositionOverlays();
    QTimer::singleShot(4000, this, [this]() {
        m_bannerNotice = false;
        if (m_clipBusy) return; // a new save started — its "Saving clip…" owns the banner
        restoreBannerTitle();
        // If the OSD already timed out, the banner was only up for the notice.
        if (titleBar && engine->isPlaying() && !osdTimer->isActive())
            titleBar->hide();
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
