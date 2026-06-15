#include "Seagull.h"
#include "Modules/UI/Theme.h"
#include "Modules/UI/SetupDialog.h"
#include "Modules/UI/Widgets/UpdateDialog.h"
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
    connect(appUpdate, &SgAppUpdate::updateAvailable, this,
        [this](const QString& v, const QString& notes, const QString& url) {
            if (m_appCheckStartup) {
                m_appCheckStartup = false;
                if (showAppUpdatePrompt(v, notes, url))
                    m_selfUpdateFromStartup = true; // halt tools; the new launch checks them
                else
                    runToolUpdateFlow();            // user deferred -> proceed to tools
                return;
            }
            m_appCheckManual = false;               // manual (Settings) path
            showAppUpdatePrompt(v, notes, url);
        });
    connect(appUpdate, &SgAppUpdate::upToDate, this, [this]() {
        if (m_appCheckStartup) { m_appCheckStartup = false; runToolUpdateFlow(); return; }
        if (!m_appCheckManual) return;
        m_appCheckManual = false;
        QMessageBox::information(mainWindow, "Seagull",
            QString("You're on the latest version (%1).").arg(QString::fromLatin1(SEAGULL_VERSION)));
    });
    connect(appUpdate, &SgAppUpdate::checkFailed, this, [this](const QString& reason) {
        // A failed app check must not block tools at startup.
        if (m_appCheckStartup) { m_appCheckStartup = false; runToolUpdateFlow(); return; }
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

    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(explorerModule, "File Explorer");
    mainWindow->addTab(queueModule, "Queue");
    mainWindow->addTab(searchModule, "Search");
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

    connect(explorerModule, &FileExplorer::playMediaRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Explorer;
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
    connect(explorerModule, &FileExplorer::enqueueRequested, queueModule, &Queue::addLocalFilesToQueue);

    // A fresh playlist file landed in the playlist folder — flash the Library tab.
    connect(queueModule, &Queue::playlistSaved, this, [this](const QString&) { flashLibraryTab(); });

    // A search result card plays through the same path as the queue. A short
    // additionally gets the YouTube feed behaviour: it loops at the end, and
    // wheel-scrolling over the video walks the search results.
    connect(searchModule, &Search::playMediaRequested, videoPlayer,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Search;
            const bool wasShorts = videoPlayer->shortsMode();
            const bool isShort   = rawUrl.toString().contains("/shorts/", Qt::CaseInsensitive);
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
            // After playVideo: starting media clears the mode, this re-arms it.
            videoPlayer->setShortsMode(isShort);
            // Entering shorts viewing drops the tab pane like YouTube's
            // immersive feed; advancing within the feed leaves the user's
            // split alone (a handle click brings the tabs back any time).
            if (isShort && !wasShorts) mainWindow->collapseTabs();
        });

    // Shorts-feed scroll: wheel over the playing short = next/previous result.
    connect(videoPlayer, &VideoPlayer::shortsScrolled, this, [this](int step) {
        if (activeSource == ActiveSource::Search) searchModule->playAdjacentResult(step);
        });

    // Search card "Queue" adds to the Queue tab; "Download" goes to the dedicated
    // download worker's FIFO (files land in the library).
    connect(searchModule, &Search::enqueueRequested, queueModule,
        [this](const QUrl& url, const QString& title) {
            queueModule->addUrlToQueue(url.toString(), title);
        });
    connect(searchModule, &Search::downloadRequested, this,
        [this](const QUrl& url, const QString& /*title*/) {
            m_downloadQueue.append(url.toString());
            pumpDownloads();
        });

    // Display "Card size" resizes the Search and Library cards live.
    connect(settingsModule, &Settings::cardWidthChanged, searchModule, &Search::setCardWidth);
    connect(settingsModule, &Settings::cardWidthChanged, libraryModule, &MediaLibrary::setCardWidth);

    // Search history wipes: the Search settings' "Clear History Now" button,
    // and the on-close auto-clear when Search/ClearHistoryOnExit is ticked.
    connect(settingsModule, &Settings::clearHistoryRequested, searchModule, &Search::clearSearchHistory);
    connect(settingsModule, &Settings::visualizerSettingsChanged, videoPlayer, &VideoPlayer::applyVisualizerSettings);

    // General "Check for Updates" button -> manual app-version check (shows an
    // "up to date" message too, unlike the silent startup check).
    connect(settingsModule, &Settings::checkForUpdatesRequested, this, [this]() {
        m_appCheckManual = true;
        appUpdate->checkForUpdate();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, searchModule, [this]() {
        QSettings s(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
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

    // Once a queued/streamed video starts, clear the URL bar (the metadata preview
    // stays up, showing the now-playing video). Library playback leaves it alone.
    connect(videoPlayer, &VideoPlayer::playbackStarted, this, [this]() {
        if (activeSource == ActiveSource::Queue) queueModule->clearUrlForPlayback();
        });

    // A finished video rolls into the next one — but anything waiting in the
    // queue outranks the grids: it plays next no matter where the finished item
    // came from (the queue's play signals re-point activeSource at Queue).
    connect(videoPlayer, &VideoPlayer::mediaEnded, this, [this]() {
        if (queueModule->playNextOrStart()) return;
        if (activeSource == ActiveSource::Library)       libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Explorer) explorerModule->playNextFile();
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

void Seagull::skipActive(int delta) {
    if (activeSource == ActiveSource::Library) {
        if (delta > 0) libraryModule->playNextFile();
        else           libraryModule->playPrevFile();
    }
    else if (activeSource == ActiveSource::Explorer) {
        if (delta > 0) explorerModule->playNextFile();
        else           explorerModule->playPrevFile();
    }
    else if (activeSource == ActiveSource::Queue) {
        if (delta > 0) queueModule->playNextQueuedItem();
        else           queueModule->playPrevQueuedItem();
    }
    else if (activeSource == ActiveSource::Search) {
        searchModule->playAdjacentResult(delta > 0 ? 1 : -1);
    }
}

bool Seagull::run() {
    // The shell must be shown BEFORE the first-run dialog runs. Exec'ing an
    // application-modal dialog before any window was shown left the whole app
    // input-dead: deferred startup work (the player's winId()/VLC hookup)
    // fired inside the dialog's nested event loop, creating the native window
    // hierarchy underneath an active modal block.
    mainWindow->show();

    // Bind the Windows media controls to the now-realized top-level window. winId()
    // forces native-window creation; SMTC is per-HWND for desktop apps.
    mediaControls->attachToWindow(reinterpret_cast<void*>(mainWindow->winId()));

    // First-run Terms of Use: must be accepted before the app is usable. Shown
    // modally over the freshly shown main window (a modal before any window shows
    // leaves the app input-dead, see above). Declining quits the app. The terms
    // text is the disclaimer doc; closing or Escape counts as declining.
    {
        QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
        if (!cfg.value("Setup/TermsAccepted", false).toBool()) {
            QDialog dlg(mainWindow);
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

    // First run (or tools missing): folder confirmation + dependency download,
    // modal over the fresh main window.
    if (SetupDialog::isNeeded()) {
        m_setupActive = true;
        SetupDialog dlg(updaterWorker, mainWindow);
        dlg.exec();
        m_setupActive = false;

        // The Library already scanned with the pre-setup default folders;
        // rescan now that the user confirmed (possibly different) paths.
        libraryModule->refresh();

        // Skip this launch's startup update check entirely: setup just drove
        // the updater itself, and probing exes that were downloaded seconds
        // ago can misread their versions (first-run self-extract + AV scan)
        // and silently re-download everything. And if the user clicked Not
        // Now, downloading behind their back would override that choice;
        // setup asks again next launch instead.
        releaseThumbnailHolds(); // fresh tools just landed; thumbnails may run
        shutdownUpdater();       // setup was the updater's job this launch, done
        return true;
    }

    // Startup updates: Seagull FIRST, then the tools. Short delay so the first
    // frame paints. The thumbnail queues stay held (an ffmpeg.exe swap must never
    // race a running grab) until finishStartupUpdates() runs at the end.
    QTimer::singleShot(250, this, [this]() { runStartupUpdates(); });
    return true;
}

void Seagull::runStartupUpdates() {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_autoUpdateStartup = cfg.value("General/AutoUpdate", true).toBool();

    if (!m_autoUpdateStartup) {
        // Off: ask once before touching the network. This single ask covers both
        // the Seagull check and (downstream) the tool check.
        const auto ans = QMessageBox::question(mainWindow, "Seagull",
            "Check for updates now?\n\nThis looks for a new version of Seagull and its tools.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (ans != QMessageBox::Yes) { finishStartupUpdates(); return; } // skip everything
    }

    // Check Seagull itself first; the handlers chain into runToolUpdateFlow()
    // unless the user chooses to update the app (then tools wait for next launch).
    m_appCheckStartup = true;
    appUpdate->checkForUpdate();
}

void Seagull::runToolUpdateFlow() {
    // AutoUpdate on -> check + install automatically. Off -> we already asked in
    // runStartupUpdates, so skipAsk goes straight to the check but still confirms
    // before installing. Locks the app while it runs (no tool swap mid-use).
    UpdateDialog dlg(updaterWorker, m_autoUpdateStartup, /*skipAsk=*/!m_autoUpdateStartup, mainWindow);
    dlg.exec();
    finishStartupUpdates();
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
    // install (preserving config.ini, search_history.txt and the Tools folder),
    // then relaunches and cleans up the staging area. Lives in temp root so the
    // staged-folder cleanup can't delete it mid-run.
    const QString helperPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("seagull_apply.ps1"));
    const QString script = QStringLiteral(
        "param([int]$ProcId,[string]$Staged,[string]$Install,[string]$Exe)\n"
        "try { Wait-Process -Id $ProcId -Timeout 30 -ErrorAction SilentlyContinue } catch {}\n"
        "Start-Sleep -Milliseconds 500\n"
        "robocopy $Staged $Install /E /XF config.ini search_history.txt /XD Tools | Out-Null\n"
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

    // Apply the saved theme before any widgets are built so the whole UI is themed.
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    Theme::apply(settings.value("Display/Theme", "Seagull").toString());

    Seagull orchestrator;
    if (!orchestrator.run()) return 0; // user declined the Terms of Use
    return app.exec();
}