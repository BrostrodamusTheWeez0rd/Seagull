#include "Seagull.h"
#include "Modules/UI/Theme.h"
#include "Modules/UI/SetupDialog.h"
#include "Modules/UI/Widgets/UpdateDialog.h"
#include "Modules/Backend/SgFavorites.h"
#include "Modules/Backend/SgPaths.h"
#include "Modules/Backend/SgThumbnailer.h"
#include <QApplication>
#include <QSettings>
#include <QCoreApplication>
#include <QTextBrowser>
#include <QTimer>
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFont>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QDateTime>

// Stamped in by the build (see CMakeLists). Fallback keeps a stray build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

Seagull::Seagull(QObject* parent) : QObject(parent) {
    // The window everything hangs off of — a shell that hosts the video player.
    mainWindow = new MainWindow();
    videoPlayer = new VideoPlayer();
    mainWindow->setVideoPlayer(videoPlayer);

    // Backend workers. Each one is a yt-dlp wrapper with a single job, so their
    // long-running processes never step on each other.
    downloaderWorker = new SgYtDlp(this);
    resolverWorker = new SgYtDlp(this);
    prefetcherWorker = new SgYtDlp(this);
    playerWorker = new SgYtDlp(this);
    downloadWorker = new SgYtDlp(this);
    searchWorker = new SgSearch(this);

    // Shared Windows OS spell checker for the Search query combo + File Explorer
    // search box. Inert if the OS/language is unsupported (fields stay plain).
    spellChecker = new SgSpellCheck(this);

    // Shared ad-strip proxy for Twitch live streams. Every worker that resolves a
    // playable stream URL gets it, so the ad-free routing applies no matter which
    // path (player probe, queue prefetch, …) produced the URL.
    hlsProxy = new SgHlsProxy(this);
    downloaderWorker->setHlsProxy(hlsProxy);
    resolverWorker->setHlsProxy(hlsProxy);
    prefetcherWorker->setHlsProxy(hlsProxy);
    playerWorker->setHlsProxy(hlsProxy);

    // One -J cache shared by every worker: a video the queue title-resolver already
    // extracted is answered from cache when the prefetcher and player ask for it,
    // instead of three separate yt-dlp launches against YouTube.
    metaCache = new SgMetaCache(this);
    for (SgYtDlp* w : { downloaderWorker, resolverWorker, prefetcherWorker, playerWorker, downloadWorker })
        w->setMetaCache(metaCache);

    // Records the currently-playing live stream (parallel ffmpeg), driven by the
    // player's Record button.
    recorder = new SgRecorder(this);

    // Windows media controls (SMTC): the OS now-playing overlay + media keys.
    // Bound to the main window's HWND in run() (after the window exists).
    mediaControls = new SgMediaControls(this);

    // App update check. At startup it runs FIRST (before the tool check): if the
    // user updates Seagull itself, the tool check is skipped and handled on the
    // fresh launch. The Settings button drives the same check manually.
    appUpdate = new SgAppUpdate(this);
    // The startup Seagull-version check is driven by the UpdateDialog now; these
    // handlers only serve the manual Settings "Check for Updates" path.
    connect(appUpdate, &SgAppUpdate::updateAvailable, this,
        [this](const QString& v, const QString& notes, const QString& url) {
            if (!m_appCheckManual) return; // startup is owned by the UpdateDialog
            m_appCheckManual = false;
            showAppUpdatePrompt(v, notes, url);
        });
    connect(appUpdate, &SgAppUpdate::upToDate, this, [this]() {
        if (!m_appCheckManual) return;
        m_appCheckManual = false;
        QMessageBox::information(mainWindow, "Seagull",
            QString("You're on the latest version (%1).").arg(QString::fromLatin1(SEAGULL_VERSION)));
    });
    connect(appUpdate, &SgAppUpdate::checkFailed, this, [this](const QString& reason) {
        if (!m_appCheckManual) return;
        m_appCheckManual = false;
        QMessageBox::warning(mainWindow, "Seagull",
            "Could not check for updates.\n\n" + reason);
    });
    // Self-update download/stage progress + completion.
    connect(appUpdate, &SgAppUpdate::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (!m_updateProgress) return;
        if (total > 0) { m_updateProgress->setMaximum(100);
                         m_updateProgress->setValue(int(got * 100 / total)); }
    });
    connect(appUpdate, &SgAppUpdate::downloadFailed, this, [this](const QString& reason) {
        if (m_updateProgress) { m_updateProgress->close(); m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed",
            "Could not download the update.\n\n" + reason +
            "\n\nYou can still update manually from the releases page.");
        // A startup self-update that failed: fall back to running normally (the
        // tool check is intentionally skipped — it'll run next launch).
        if (m_selfUpdateFromStartup) { m_selfUpdateFromStartup = false; finishStartupUpdates(); }
    });
    connect(appUpdate, &SgAppUpdate::readyToApply, this, &Seagull::onUpdateReadyToApply);

    // The tab modules.
    libraryModule = new MediaLibrary(spellChecker);
    explorerModule = new FileExplorer(spellChecker);
    queueModule = new Queue(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search(searchWorker, spellChecker);
    settingsModule = new Settings();
    eqModule = new EQ();

    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(explorerModule, "File Explorer");
    mainWindow->addTab(queueModule, "Queue");
    mainWindow->addTab(searchModule, "Search");
    mainWindow->addTab(eqModule, "EQ");
    mainWindow->addTab(settingsModule, "Settings");

    // --- Description tab + Share button (replaced the banner's Info/Share) ---
    // The Description page appears as a dynamic tab whenever the playing video's
    // probe reports a description, and retires with it (local files, teardown).
    descriptionView = new QTextBrowser();
    descriptionView->setOpenExternalLinks(true);
    descriptionView->setReadOnly(true);

    connect(videoPlayer, &VideoPlayer::videoInfoChanged, this,
        [this](const QString& title, const QString& uploader, const QString& views,
               const QString& date, const QString& description) {
            if (description.trimmed().isEmpty()) {
                mainWindow->closeDynamicTab(descriptionView);
                return;
            }
            QStringList bits;
            if (!uploader.isEmpty())                 bits << uploader;
            if (!views.isEmpty() && views != "0")    bits << views + " views";
            if (!date.isEmpty())                     bits << date;
            descriptionView->setHtml(
                "<h3>" + title.toHtmlEscaped() + "</h3>"
                + (bits.isEmpty() ? QString()
                    : "<p>" + bits.join(QStringLiteral("   |   ")).toHtmlEscaped() + "</p><hr>")
                + "<p style=\"white-space: pre-wrap;\">" + description.toHtmlEscaped() + "</p>");
            mainWindow->openDynamicTab(descriptionView, "Description");
        });
    connect(videoPlayer, &VideoPlayer::shareAvailableChanged, mainWindow, &MainWindow::setShareAvailable);
    connect(mainWindow, &MainWindow::shareRequested, videoPlayer, &VideoPlayer::shareLink);

    // When a module wants something played, it tells the window. We remember which
    // source asked, so "play next" later knows whether to walk the library, the
    // explorer's file list, or the queue.
    connect(libraryModule, &MediaLibrary::playMediaRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Library;
        videoPlayer->playLocalFile(url);
        });

    connect(queueModule, &Queue::playMediaRequested, videoPlayer,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Queue;
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

    // A local queue row plays through the player's local path; activeSource stays
    // Queue so auto-advance walks the queue, not the Library grid.
    connect(queueModule, &Queue::playLocalFileRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Queue;
        videoPlayer->playLocalFile(url);
        });

    // Library "Playlists" card -> the Queue loads the .sgpl and starts playing it.
    connect(libraryModule, &MediaLibrary::playPlaylistRequested, queueModule, [this](const QString& path) {
        queueModule->loadPlaylistFile(path, true);
        });

    // Local files queued from Library cards / the File Explorer context menu.
    // The queue itself enforces the local/online split (clear-first modal).
    connect(libraryModule, &MediaLibrary::enqueueLocalRequested, queueModule, &Queue::addLocalFilesToQueue);

    // While the Library builds its card grid (tab/category switch), pause the audio
    // visualizer's render timer so the two don't fight over the GUI thread (the hitch
    // only showed while local audio was playing — i.e. the visualizer was up).
    connect(libraryModule, &MediaLibrary::buildBusyChanged,
            videoPlayer, &VideoPlayer::setVisualizerSuspended);

    // A fresh playlist file landed in the playlist folder — flash the Library tab.
    connect(queueModule, &Queue::playlistSaved, this, [this](const QString&) { flashLibraryTab(); });

    // Shorts-feed scroll: wheel over the playing short = next/previous result of
    // whichever search tab is the active feed. (Search card play wiring lives in
    // wireSearchTab so every search tab — primary or duplicate — behaves the same.)
    connect(videoPlayer, &VideoPlayer::shortsScrolled, this, [this](int step) {
        if (activeSource == ActiveSource::Search && m_activeSearch)
            m_activeSearch->playAdjacentResult(step);
        });

    // Display "Card size" resizes the Library cards live. Search cards are handled
    // per-tab in wireSearchTab (there can be several Search tabs).
    connect(settingsModule, &Settings::cardWidthChanged, libraryModule, &MediaLibrary::setCardWidth);
    connect(settingsModule, &Settings::visualizerSettingsChanged, videoPlayer, &VideoPlayer::applyVisualizerSettings);

    // EQ tab live edits: apply to the player ONLY when the playing media's kind
    // matches the edited content type. Otherwise the EQ tab has already persisted it
    // (config Eq/<type>/*) and VideoPlayer auto-applies the saved curve when that kind
    // next plays — so editing the Audio EQ never disturbs a currently-playing video.
    connect(eqModule, &EQ::eqChanged, this,
        [this](EqContentType type, const QVector<float>& gains, float preampDb) {
            const MediaKind k = videoPlayer->currentMediaKind();
            const bool matches = (type == EqContentType::Audio && k == MediaKind::Audio)
                              || (type == EqContentType::Video && k == MediaKind::Video);
            if (matches) videoPlayer->applyEqualizer(gains, preampDb);
        });

    // EQ power toggle: apply the curve (on) or bypass the equalizer (off) live, but
    // only when the playing media's kind matches the toggled type. Otherwise it's
    // already persisted (Eq/<type>/Enabled) and VideoPlayer honours it on next play.
    connect(eqModule, &EQ::eqEnabledChanged, this,
        [this](EqContentType type, bool enabled, const QVector<float>& gains, float preampDb) {
            const MediaKind k = videoPlayer->currentMediaKind();
            const bool matches = (type == EqContentType::Audio && k == MediaKind::Audio)
                              || (type == EqContentType::Video && k == MediaKind::Video);
            if (!matches) return;
            if (enabled) videoPlayer->applyEqualizer(gains, preampDb);
            else         videoPlayer->disableEqualizer();
        });

    // Multiple-instance tabs. The primary Search + File Explorer go through the same
    // per-tab wiring the duplicates use; register them as duplicable so the "+" menu
    // offers "New Search tab" / "New File Explorer tab", and wire the open/close hooks.
    m_searchTabs.append(searchModule);     m_activeSearch   = searchModule;   wireSearchTab(searchModule);
    m_explorerTabs.append(explorerModule); m_activeExplorer = explorerModule; wireExplorerTab(explorerModule);
    mainWindow->registerDuplicableTab("Search", "New Search tab");
    mainWindow->registerDuplicableTab("File Explorer", "New File Explorer tab");
    connect(mainWindow, &MainWindow::newTabRequested,   this,
        [this](const QString& kind) { openDuplicateTab(kind); }); // "+" menu: switch to the new tab
    connect(mainWindow, &MainWindow::duplicateTabClosed, this, &Seagull::disposeDuplicateTab);

    // General "Check for Updates" button -> manual app-version check (shows an
    // "up to date" message too, unlike the silent startup check).
    connect(settingsModule, &Settings::checkForUpdatesRequested, this, [this]() {
        m_appCheckManual = true;
        appUpdate->checkForUpdate();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, searchModule, [this]() {
        QSettings s(SgPaths::configFile(), QSettings::IniFormat);
        if (s.value("Search/ClearHistoryOnExit", false).toBool())
            searchModule->clearSearchHistory();
        });

    // Each finished ad-hoc download advances the FIFO; the Library spinner stays up
    // until the queue drains.
    connect(downloadWorker, &SgYtDlp::finished, this, [this](bool /*ok*/) {
        if (!m_downloadQueue.isEmpty()) m_downloadQueue.removeFirst();
        m_downloading = false;
        if (!m_downloadQueue.isEmpty()) pumpDownloads();
        else mainWindow->setTabBusy(libraryModule, false);
        });

    // Surface the search/download workers' logs in the same dev console as the rest.
    connect(searchWorker, &SgSearch::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);
    connect(downloadWorker, &SgYtDlp::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // Any worker that hits a bot-check / throttling block warns the user (debounced).
    // Queued so the modal opens from the event loop, not re-entrantly inside the
    // worker's finished-handler while it's mid-emit.
    for (SgYtDlp* w : { downloaderWorker, resolverWorker, prefetcherWorker, playerWorker, downloadWorker })
        connect(w, &SgYtDlp::extractionBlocked, this, &Seagull::onExtractionBlocked, Qt::QueuedConnection);

    // Once a queued/streamed video starts, clear the URL bar (the metadata preview
    // stays up, showing the now-playing video). Library playback leaves it alone.
    connect(videoPlayer, &VideoPlayer::playbackStarted, this, [this]() {
        if (activeSource == ActiveSource::Queue) queueModule->clearUrlForPlayback();
        // Point the autoplay/shuffle toggles at this content type and arm the
        // slideshow if a photo just loaded.
        m_currentIsPhoto = (videoPlayer->currentMediaKind() == MediaKind::Photo);
        mainWindow->setPlaybackContext(currentContextKey(), m_currentIsPhoto);
        updateSlideshow();
        });

    // A finished video rolls into the next one — but anything waiting in the
    // queue outranks the grids: it plays next no matter where the finished item
    // came from (the queue's play signals re-point activeSource at Queue). When
    // shuffle is on, the next pick from each source is random.
    connect(videoPlayer, &VideoPlayer::mediaEnded, this, [this]() {
        if (!mainWindow->autoplayEnabled()) return;
        const bool shuffle = mainWindow->shuffleEnabled();
        if (shuffle ? queueModule->playRandomOrStart() : queueModule->playNextOrStart()) return;
        if (activeSource == ActiveSource::Library)
            shuffle ? libraryModule->playRandomFile() : libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Explorer && m_activeExplorer)
            shuffle ? m_activeExplorer->playRandomFile() : m_activeExplorer->playNextFile();
        else if (activeSource == ActiveSource::Search && m_activeSearch)
            shuffle ? m_activeSearch->playRandomResult() : m_activeSearch->playAdjacentResult(1);
        });

    // The skip buttons (single-click = nudge, double-click = jump tracks) land here,
    // and so do the SMTC next/previous keys (see skipActive).
    connect(videoPlayer, &VideoPlayer::skipRequested, this, [this](int delta) { skipActive(delta); });

    // --- Windows media controls (SMTC) ---
    // Player -> OS: mirror state, metadata and artwork into the now-playing widget.
    connect(videoPlayer, &VideoPlayer::playbackStarted, mediaControls, [this]() {
        mediaControls->setEnabled(true);
        // Optimistic Playing so the session is active before metadata lands; the
        // engine's real playing/paused signal corrects it a beat later.
        mediaControls->setPlaybackStatus(SgMediaControls::Status::Playing);
        });
    connect(videoPlayer, &VideoPlayer::smtcStateChanged, mediaControls, [this](int state) {
        switch (state) {
        case 1: mediaControls->setPlaybackStatus(SgMediaControls::Status::Playing); break;
        case 2: mediaControls->setPlaybackStatus(SgMediaControls::Status::Paused);  break;
        default: mediaControls->setPlaybackStatus(SgMediaControls::Status::Stopped); break;
        }
        });
    connect(videoPlayer, &VideoPlayer::smtcMetadata, mediaControls, &SgMediaControls::setMetadata);
    connect(videoPlayer, &VideoPlayer::smtcArtwork,  mediaControls, &SgMediaControls::setThumbnail);
    connect(videoPlayer, &VideoPlayer::closed, mediaControls, [this]() { mediaControls->clear(); });

    // OS -> player: media keys / overlay buttons drive playback.
    connect(mediaControls, &SgMediaControls::playPressed,     videoPlayer, &VideoPlayer::togglePlayPause);
    connect(mediaControls, &SgMediaControls::pausePressed,    videoPlayer, &VideoPlayer::togglePlayPause);
    connect(mediaControls, &SgMediaControls::nextPressed,     this, [this]() { skipActive(1); });
    connect(mediaControls, &SgMediaControls::previousPressed, this, [this]() { skipActive(-1); });

    // Push the timeline (position/duration) to the overlay scrubber while playing.
    smtcTimelineTimer = new QTimer(this);
    smtcTimelineTimer->setInterval(1000);
    connect(smtcTimelineTimer, &QTimer::timeout, this, [this]() {
        if (videoPlayer->hasActiveMedia())
            mediaControls->setTimeline(videoPlayer->mediaPosition(), videoPlayer->mediaDuration());
        });
    smtcTimelineTimer->start();

    // Photo slideshow: advance to the next photo when the interval elapses. Each
    // new photo's playbackStarted re-arms it (see updateSlideshow); landing on the
    // last photo simply leaves the one-shot timer stopped, so the show ends there.
    slideshowTimer = new QTimer(this);
    slideshowTimer->setSingleShot(true);
    connect(slideshowTimer, &QTimer::timeout, this, [this]() { skipActive(1); });
    // Toggling the slideshow control, or editing the interval, re-evaluates it.
    connect(mainWindow, &MainWindow::autoplayChanged,      this, [this](bool) { updateSlideshow(); });
    connect(mainWindow, &MainWindow::photoIntervalChanged, this, [this](int)  { updateSlideshow(); });
    // Tearing down playback ends any running slideshow.
    connect(videoPlayer, &VideoPlayer::closed, this, [this]() {
        m_currentIsPhoto = false;
        slideshowTimer->stop();
        });

    // Surface the player worker's logs (stream resolution, yt-dlp errors) in the
    // same dev console as the others, by re-emitting through the downloader.
    connect(playerWorker, &SgYtDlp::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // The player's stop/EOF poster for LOCAL files: a dedicated thumbnailer
    // (it shares the disk cache with the Library's, so anything the Library
    // already thumbed answers instantly) grabs a frame / cover art per play.
    playerThumbnailer = new SgThumbnailer(this);
    connect(videoPlayer, &VideoPlayer::localPosterRequested,
            playerThumbnailer, &SgThumbnailer::requestThumbnail);
    connect(playerThumbnailer, &SgThumbnailer::thumbnailReady,
            videoPlayer, &VideoPlayer::onLocalPosterReady);

    // Hold every thumbnail ffmpeg queue until the startup update modal is done:
    // updates run FIRST now (the modal locks the app), and an ffmpeg.exe swap
    // must never race a running grab. releaseThumbnailHolds() lifts both.
    libraryModule->setThumbnailsHeld(true);
    playerThumbnailer->setHeld(true);

    // Player asks the backend to resolve qualities and stream URLs on demand.
    connect(videoPlayer, &VideoPlayer::probeQualitiesRequested, playerWorker, &SgYtDlp::probeAvailableQualities);

    connect(videoPlayer, &VideoPlayer::streamUrlRequested, playerWorker, [this](const QString& url, const QString& formatId, bool freshResolve) {
        playerWorker->cancel(); // free the worker (e.g. drop an in-flight quality probe) so the resolve runs now
        playerWorker->fetchMetadataAndStreamUrl(url, formatId, freshResolve);
        });

    // Results come back on queued connections so they always land on the UI thread.
    connect(playerWorker, &SgYtDlp::availableQualitiesFound, videoPlayer, &VideoPlayer::handleAvailableQualities, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::liveStatusKnown, videoPlayer, &VideoPlayer::onLiveStatus, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::thumbnailResolved, videoPlayer, &VideoPlayer::onThumbnailResolved, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::videoInfoReady, videoPlayer, &VideoPlayer::onVideoInfo, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::streamUrlReady, videoPlayer, &VideoPlayer::onStreamUrlReady, Qt::QueuedConnection);

    // --- Live-stream recording (parallel ffmpeg against the same resolved URLs) ---
    connect(videoPlayer, &VideoPlayer::recordStartRequested, recorder,
        [this](const QUrl& videoUrl, const QUrl& audioUrl, const QString& referer, const QString& title) {
            // The URL VLC plays is already the (ad-free, for Twitch) stream; record it
            // verbatim. Referer is the page URL (ignored for Twitch's localhost proxy
            // URL, but helps hotlink-protected CDNs on other live sites).
            recorder->start(videoUrl, audioUrl, referer, title);
        });
    connect(videoPlayer, &VideoPlayer::recordStopRequested, recorder, &SgRecorder::stop);
    // VOD: clip the watched range [startMs,endMs] — direct ffmpeg cut of the resolved
    // stream URLs, with a yt-dlp full-download fallback driven off the page URL.
    connect(videoPlayer, &VideoPlayer::recordClipRequested, recorder,
        [this](const QString& pageUrl, const QUrl& videoUrl, const QUrl& audioUrl,
            qint64 startMs, qint64 endMs, const QString& title) {
            recorder->clipSection(pageUrl, videoUrl, audioUrl, startMs, endMs, title);
        });
    connect(videoPlayer, &VideoPlayer::recordClipCancelRequested, recorder, &SgRecorder::cancelClip);

    connect(recorder, &SgRecorder::started, videoPlayer, [this](const QString&) { videoPlayer->onRecordingStarted(); }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::finished, videoPlayer, [this](const QString& file, bool ok) {
        videoPlayer->onRecordingStopped(file, ok);
        if (ok && !file.isEmpty()) flashLibraryTab(); // the recording is on disk + playable
        }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::clipFinished, videoPlayer, [this](const QString& file, bool ok) {
        videoPlayer->onClipFinished(file, ok);
        if (ok && !file.isEmpty()) flashLibraryTab(); // the clip is on disk + playable
        }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // --- Tool auto-update, off the main thread ---
    // Checking and downloading yt-dlp / Deno / ffmpeg means blocking network calls,
    // SHA-256 hashing, and unzipping that can take many seconds. Running that on the
    // UI thread froze startup, so the updater lives on its own thread instead. The
    // worker has no parent (a requirement for moveToThread); its child QProcess and
    // network manager move across with it automatically.
    updaterThread = new QThread(this);
    updaterWorker = new SgUpdater(nullptr);
    updaterWorker->moveToThread(updaterThread);

    // Status lines still show up in the Queue log, hopping back to the UI thread.
    connect(updaterWorker, &SgUpdater::updateStatus, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // Tear the worker down with its thread.
    connect(updaterThread, &QThread::finished, updaterWorker, &QObject::deleteLater);

    updaterThread->start();

    // The startup check/install is driven by the modal UpdateDialog in run();
    // it owns checkFinished/applyProgress/applyFinished for the whole flow.
}

void Seagull::wireSearchTab(Search* s) {
    // A search card plays through the same path as the queue; a short additionally
    // loops at the end and the wheel walks the feed. Capturing `s` lets the active
    // feed (skip / shorts-scroll) follow whichever search tab started playback.
    connect(s, &Search::playMediaRequested, videoPlayer,
        [this, s](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource   = ActiveSource::Search;
            m_activeSearch = s;
            const bool wasShorts = videoPlayer->shortsMode();
            const bool isShort   = rawUrl.toString().contains("/shorts/", Qt::CaseInsensitive);
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
            videoPlayer->setShortsMode(isShort); // playVideo cleared it — re-arm
            if (isShort && !wasShorts) mainWindow->collapseTabs();
        });
    // Card "Queue" adds to the Queue tab; "Download" goes to the dedicated download
    // worker's FIFO (files land in the library).
    connect(s, &Search::enqueueRequested, queueModule,
        [this](const QUrl& url, const QString& title) { queueModule->addUrlToQueue(url.toString(), title); });
    connect(s, &Search::downloadRequested, this,
        [this](const QUrl& url, const QString& /*title*/) { m_downloadQueue.append(url.toString()); pumpDownloads(); });
    // Live "Card size" + the Search settings' "Clear History Now" reach every tab.
    connect(settingsModule, &Settings::cardWidthChanged,      s, &Search::setCardWidth);
    connect(settingsModule, &Settings::clearHistoryRequested, s, &Search::clearSearchHistory);
}

void Seagull::wireExplorerTab(FileExplorer* e) {
    connect(e, &FileExplorer::playMediaRequested, videoPlayer, [this, e](const QUrl& url) {
        activeSource     = ActiveSource::Explorer;
        m_activeExplorer = e; // this tab's file list is what skip walks
        videoPlayer->playLocalFile(url);
        });
    connect(e, &FileExplorer::enqueueRequested, queueModule, &Queue::addLocalFilesToQueue);
}

void Seagull::openDuplicateTab(const QString& kind, bool switchTo) {
    // "+" menu -> a fresh extra instance. Each Search duplicate gets its OWN SgSearch
    // worker (true concurrent searching — which is exactly why Search warns about the
    // bot risk of two tabs on one site). File Explorer has no worker.
    if (kind == QStringLiteral("Search")) {
        auto* worker = new SgSearch(this);
        connect(worker, &SgSearch::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);
        auto* s = new Search(worker, spellChecker);
        m_searchWorkers.insert(s, worker);
        m_searchTabs.append(s);
        wireSearchTab(s);
        mainWindow->addDuplicateTab(s, "Search", switchTo);
    }
    else if (kind == QStringLiteral("File Explorer")) {
        auto* e = new FileExplorer(spellChecker);
        m_explorerTabs.append(e);
        wireExplorerTab(e);
        mainWindow->addDuplicateTab(e, "File Explorer", switchTo);
    }
}

void Seagull::disposeDuplicateTab(QWidget* page) {
    // A duplicate tab closed: delete the instance (and its worker). The active-feed
    // pointer falls back to the primary so skip/scroll still have a valid target.
    if (auto* s = qobject_cast<Search*>(page)) {
        m_searchTabs.removeOne(s);
        if (m_activeSearch == s) m_activeSearch = searchModule;
        if (SgSearch* w = m_searchWorkers.take(s)) w->deleteLater();
        s->deleteLater();
    }
    else if (auto* e = qobject_cast<FileExplorer*>(page)) {
        m_explorerTabs.removeOne(e);
        if (m_activeExplorer == e) m_activeExplorer = explorerModule;
        e->deleteLater();
    }
}

void Seagull::releaseThumbnailHolds() {
    libraryModule->setThumbnailsHeld(false);
    playerThumbnailer->setHeld(false);
}

void Seagull::shutdownUpdater() {
    // Safe to call as soon as the setup/update dialog has closed: the dialogs
    // only close after applyFinished (or with nothing started), so the worker
    // is idle. finished -> deleteLater (wired in the ctor) frees the worker on
    // its own thread as it winds down.
    if (!updaterThread) return;
    updaterThread->quit();
    updaterThread->wait();
    updaterThread->deleteLater();
    updaterThread = nullptr;
    updaterWorker = nullptr; // deleted via the finished->deleteLater connect
}

Seagull::~Seagull() {
    // Stop the updater thread cleanly before anything else goes away.
    if (updaterThread) {
        updaterThread->quit();
        updaterThread->wait();
    }
    delete mainWindow;
}

void Seagull::pumpDownloads() {
    if (m_downloading || m_downloadQueue.isEmpty()) return;
    m_downloading = true;
    mainWindow->setTabBusy(libraryModule, true); // spin the Library tab while downloading
    downloadWorker->download(m_downloadQueue.first());
}

void Seagull::flashLibraryTab() {
    // Brief seagull on the Library tab: a recording/clip just landed and is playable.
    mainWindow->setTabBusy(libraryModule, true);
    QTimer::singleShot(4000, this, [this]() {
        // Don't clear a spinner a still-draining download queue owns.
        if (m_downloadQueue.isEmpty()) mainWindow->setTabBusy(libraryModule, false);
        });
}

void Seagull::onExtractionBlocked(const QString& kind, const QString& detail) {
    // Debounce: several workers (resolver, prefetcher, player) can trip on the same
    // block within moments, and yt-dlp retries recur. Show one modal, then stay quiet
    // for a cooldown so the user isn't buried in identical dialogs.
    constexpr qint64 kCooldownMs = 60'000; // one minute between warnings
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_blockWarnActive || (m_lastBlockWarnMs && now - m_lastBlockWarnMs < kCooldownMs))
        return;
    m_blockWarnActive = true;

    QString title, body;
    if (kind == "throttle") {
        title = "Connection throttled";
        body  = "The site is rate-limiting requests right now (HTTP 429), so playback "
                "or downloads may fail or stall.\n\n"
                "This usually clears up on its own. Wait a few minutes before trying "
                "again, and avoid starting lots of videos or downloads at once.";
    } else {
        title = "Verification required";
        body  = "The site is asking Seagull to confirm it's not a bot, so it won't "
                "hand over the video right now.\n\n"
                "This often passes if you wait a little and try again. If it keeps "
                "happening, it's coming from the site, not from anything wrong on "
                "your end.";
    }
    if (!detail.trimmed().isEmpty())
        body += "\n\nDetails:\n" + detail.trimmed();

    QMessageBox::warning(mainWindow, title, body);

    m_lastBlockWarnMs = QDateTime::currentMSecsSinceEpoch();
    m_blockWarnActive = false;
}

void Seagull::skipActive(int delta) {
    // Forward skips honour shuffle (random next); backward always steps in order.
    const bool shuffle = delta > 0 && mainWindow->autoplayEnabled()
                                   && mainWindow->shuffleEnabled();
    if (activeSource == ActiveSource::Library) {
        if (shuffle)        libraryModule->playRandomFile();
        else if (delta > 0) libraryModule->playNextFile();
        else                libraryModule->playPrevFile();
    }
    else if (activeSource == ActiveSource::Explorer) {
        if (m_activeExplorer) {
            if (shuffle)        m_activeExplorer->playRandomFile();
            else if (delta > 0) m_activeExplorer->playNextFile();
            else                m_activeExplorer->playPrevFile();
        }
    }
    else if (activeSource == ActiveSource::Queue) {
        if (shuffle)        queueModule->playRandomOrStart();
        else if (delta > 0) queueModule->playNextQueuedItem();
        else                queueModule->playPrevQueuedItem();
    }
    else if (activeSource == ActiveSource::Search) {
        if (shuffle && m_activeSearch)      m_activeSearch->playRandomResult();
        else if (m_activeSearch)            m_activeSearch->playAdjacentResult(delta > 0 ? 1 : -1);
    }
}

QString Seagull::currentContextKey() const {
    switch (activeSource) {
    case ActiveSource::Library:  return libraryModule->sessionContextKey();
    case ActiveSource::Explorer: return QStringLiteral("explorer");
    case ActiveSource::Queue:    return QStringLiteral("queue");
    case ActiveSource::Search:   return m_activeSearch ? m_activeSearch->playbackContextKey()
                                                       : QStringLiteral("search.youtube");
    case ActiveSource::None:     break;
    }
    return QStringLiteral("queue"); // direct URL paste etc. -> queue-style context
}

void Seagull::updateSlideshow() {
    // Run the slideshow only for a photo with autoplay (slideshow) enabled.
    if (m_currentIsPhoto && mainWindow->autoplayEnabled()) {
        slideshowTimer->start(mainWindow->photoIntervalSeconds() * 1000);
    } else {
        slideshowTimer->stop();
    }
}

bool Seagull::run() {
    // The player's VLC output HWND is bound AFTER mainWindow->show() below — the
    // proven timing. What lets the startup modals run while the window is still
    // hidden is that the player no longer queues a deferred winId()/VLC hookup at
    // construction (it was a QTimer::singleShot(0)). That stray deferred call was
    // the landmine: it could fire inside a modal's nested event loop and realize
    // the native windows under an active modal block, leaving the app input-dead.
    // With it gone, nothing touches winId() while a pre-window modal is up.

    // Eagerly construct the favorites singleton so it loads its JSON before any
    // VideoCard is built (the singleton is safe to call before this, but this
    // guarantees the load happens on the main thread before modules are shown).
    SgFavorites::instance();

    // First-run Terms of Use: must be accepted before the app is usable. Shown
    // modally BEFORE the window (safe now that the player's deferred winId hookup
    // is gone). Declining quits the app. Closing or Escape counts as declining.
    {
        QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
        if (!cfg.value("Setup/TermsAccepted", false).toBool()) {
            QDialog dlg(nullptr);
            dlg.setWindowTitle("Seagull - Terms of Use");
            dlg.resize(560, 480);
            auto* lay = new QVBoxLayout(&dlg);
            auto* view = new QTextBrowser(&dlg);
            view->setOpenExternalLinks(true);
            QFile f(":/docs/DISCLAIMER.md");
            view->setMarkdown(f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString());
            lay->addWidget(view);
            auto* buttons = new QDialogButtonBox(&dlg);
            buttons->addButton("I Agree", QDialogButtonBox::AcceptRole);
            buttons->addButton("Decline", QDialogButtonBox::RejectRole);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            lay->addWidget(buttons);
            if (dlg.exec() != QDialog::Accepted) return false; // declined -> main() exits
            cfg.setValue("Setup/TermsAccepted", true);
            cfg.sync();
        }
    }

    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    m_autoUpdateStartup = cfg.value("General/AutoUpdate", true).toBool();
    const bool firstRunTools = SetupDialog::isNeeded();

    // Two-stage updater modal, BEFORE the window (the window stays hidden until it
    // closes), but ONLY when AutoUpdate is on. AutoUpdate now means "ask on startup,
    // then run the check on Yes" — it never installs without a prompt. Off skips the
    // startup check entirely; the Settings "Check for Updates" button is the only
    // path then. Stage 1 checks Seagull; stage 2 checks the tools — except on first
    // run, where SetupDialog below is the tool stage (runToolStage=false), which
    // also sidesteps re-probing freshly-extracted tool exes (they'd misread their
    // versions and silently re-download). The thumbnail ffmpeg queues stay held
    // until finishStartupUpdates() so a tool swap can't race a running grab.
    m_selfUpdateChosen = false;
    if (m_autoUpdateStartup) {
        UpdateDialog dlg(appUpdate, updaterWorker, /*autoInstall=*/true,
                         /*skipAsk=*/false, /*runToolStage=*/!firstRunTools, nullptr);
        connect(&dlg, &UpdateDialog::selfUpdateRequested, this,
                [this]() { m_selfUpdateChosen = true; });
        dlg.exec();
    }

    if (m_selfUpdateChosen) {
        // The user accepted a Seagull update at stage 1. Show the window, then run
        // the self-update (its own progress dialog); it relaunches on success, or
        // falls back to running normally on cancel/failure (finishStartupUpdates).
        // The tool/Setup stage waits for the fresh launch.
        m_selfUpdateFromStartup = true;
        mainWindow->show();
        videoPlayer->rebindOutputWindow(); // bind VLC now the render HWND exists
        mediaControls->attachToWindow(reinterpret_cast<void*>(mainWindow->winId()));
        startSelfUpdate();
        return true;
    }

    // First run (or tools missing): folder confirmation + dependency download.
    // This is stage 2 on first run, shown before the window like the rest.
    if (firstRunTools) {
        m_setupActive = true;
        SetupDialog setup(updaterWorker, nullptr);
        setup.exec();
        m_setupActive = false;

        // The Library already scanned with the pre-setup default folders;
        // rescan now that the user confirmed (possibly different) paths.
        libraryModule->refresh();
    }

    // Now reveal the window, bind VLC's output to the render frame's HWND, and bind
    // the Windows media controls to the window HWND (SMTC is per-HWND for desktop
    // apps).
    mainWindow->show();
    videoPlayer->rebindOutputWindow(); // bind VLC now the render HWND exists
    mediaControls->attachToWindow(reinterpret_cast<void*>(mainWindow->winId()));

    for (const QString& kind : cfg.value("Tabs/ExtraTabs").toStringList())
        openDuplicateTab(kind, false /*switchTo*/);

    finishStartupUpdates(); // release thumbnail holds + shut the updater thread
    return true;
}

void Seagull::finishStartupUpdates() {
    releaseThumbnailHolds();
    shutdownUpdater(); // the startup flow was the updater's whole job
}

bool Seagull::showAppUpdatePrompt(const QString& version, const QString& notes, const QString& pageUrl) {
    QDialog dlg(mainWindow);
    dlg.setWindowTitle("Update Available");
    dlg.resize(520, 460);
    auto* lay = new QVBoxLayout(&dlg);

    auto* heading = new QLabel(
        QString("Seagull %1 is available. You have %2.")
            .arg(version, QString::fromLatin1(SEAGULL_VERSION)), &dlg);
    QFont hf = heading->font();
    hf.setBold(true);
    hf.setPointSize(hf.pointSize() + 1);
    heading->setFont(hf);
    heading->setWordWrap(true);
    lay->addWidget(heading);

    auto* notesView = new QTextBrowser(&dlg);
    notesView->setOpenExternalLinks(true);
    if (notes.trimmed().isEmpty()) notesView->setPlainText("No release notes were provided.");
    else                           notesView->setMarkdown(notes);
    lay->addWidget(notesView, 1);

    auto* buttons = new QDialogButtonBox(&dlg);
    auto* update = buttons->addButton("Update Now", QDialogButtonBox::AcceptRole);
    auto* page   = buttons->addButton("View on GitHub", QDialogButtonBox::ActionRole);
    buttons->addButton("Later", QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(page, &QPushButton::clicked, &dlg, [pageUrl]() { QDesktopServices::openUrl(QUrl(pageUrl)); });
    update->setDefault(true);
    lay->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        startSelfUpdate(); // download + stage + swap, in-app
        return true;
    }
    return false; // user chose Later / View on GitHub (and closed)
}

void Seagull::startSelfUpdate() {
    m_updateProgress = new QProgressDialog("Downloading update...", "Cancel", 0, 0, mainWindow);
    m_updateProgress->setWindowTitle("Updating Seagull");
    m_updateProgress->setWindowModality(Qt::ApplicationModal);
    m_updateProgress->setMinimumDuration(0);
    m_updateProgress->setAutoClose(false);
    m_updateProgress->setAutoReset(false);
    // Cancel just abandons the in-flight download; the install hasn't been touched.
    connect(m_updateProgress, &QProgressDialog::canceled, this, [this]() {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        // If this was the startup self-update, fall back to running normally
        // (release the thumbnail holds + shut the updater; tools wait for next launch).
        if (m_selfUpdateFromStartup) { m_selfUpdateFromStartup = false; finishStartupUpdates(); }
    });
    m_updateProgress->show();
    appUpdate->downloadAndApply();
}

void Seagull::onUpdateReadyToApply(const QString& stagedAppDir) {
    if (!m_updateProgress) return; // user canceled during download — don't apply

    m_updateProgress->setLabelText("Installing update...");
    m_updateProgress->setCancelButton(nullptr); // past the point of no return now

    const QString installDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString exePath    = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString staged     = QDir::toNativeSeparators(stagedAppDir);
    const qint64  pid        = QCoreApplication::applicationPid();

    // The swap helper: waits for us to exit, copies the staged build over the
    // install (preserving the Config folder and the Tools folder),
    // then relaunches and cleans up the staging area. Lives in temp root so the
    // staged-folder cleanup can't delete it mid-run.
    const QString helperPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("seagull_apply.ps1"));
    const QString script = QStringLiteral(
        "param([int]$ProcId,[string]$Staged,[string]$Install,[string]$Exe)\n"
        "try { Wait-Process -Id $ProcId -Timeout 30 -ErrorAction SilentlyContinue } catch {}\n"
        "Start-Sleep -Milliseconds 500\n"
        "robocopy $Staged $Install /E /XD Tools Config | Out-Null\n"
        "Start-Process -FilePath $Exe\n"
        "Remove-Item -LiteralPath (Split-Path $Staged -Parent) -Recurse -Force -ErrorAction SilentlyContinue\n");

    QFile f(helperPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed", "Could not prepare the update helper.");
        return;
    }
    f.write(script.toUtf8());
    f.close();

    const QStringList args = {
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
        "-File", QDir::toNativeSeparators(helperPath),
        "-ProcId", QString::number(pid),
        "-Staged", staged, "-Install", installDir, "-Exe", exePath
    };
    if (!QProcess::startDetached("powershell", args)) {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed", "Could not launch the update helper.");
        return;
    }

    // Hand off: quit so the helper can replace our files and relaunch us.
    qApp->quit();
}

int main(int argc, char* argv[]) {
    // Stamp the process with an AppUserModelID before anything else, so Windows can
    // attribute our SMTC session (otherwise the now-playing card shows no metadata).
    SgMediaControls::registerAppIdentity();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Seagull"));
    QApplication::setApplicationVersion(QString::fromLatin1(SEAGULL_VERSION));

    // Migrate flat config files into Config/ if upgrading from a pre-0.14 install.
    // This must happen before any QSettings is opened so the settings are found at the new path.
    {
        const QString appDir    = QCoreApplication::applicationDirPath();
        const QString configDir = SgPaths::configDir();
        if (!QDir(configDir).exists() && QFile::exists(appDir + "/config.ini")) {
            QDir().mkpath(configDir);
            for (const QString& name : { "config.ini", "search_history.txt",
                                          "search_history_ph.txt", "search_history_cb.txt" }) {
                const QString src = appDir + "/" + name;
                if (QFile::exists(src))
                    QFile::rename(src, configDir + "/" + name);
            }
        }
    }
    // Ensure the Config directory exists even on a clean install (migration above handles
    // upgrades; mkpath is a no-op when the folder is already there).
    QDir().mkpath(SgPaths::configDir());

    // Apply the saved theme before any widgets are built so the whole UI is themed.
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    Theme::apply(settings.value("Display/Theme", "Seagull").toString());

    Seagull orchestrator;
    if (!orchestrator.run()) return 0; // user declined the Terms of Use
    return app.exec();
}