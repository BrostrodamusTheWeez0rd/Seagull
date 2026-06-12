#include "Seagull.h"
#include "Modules/UI/Theme.h"
#include "Modules/UI/SetupDialog.h"
#include "Modules/UI/Widgets/UpdateDialog.h"
#include <QApplication>
#include <QSettings>
#include <QCoreApplication>
#include <QTextBrowser>
#include <QTimer>

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

    // The tab modules.
    libraryModule = new MediaLibrary();
    explorerModule = new FileExplorer();
    queueModule = new Queue(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search(searchWorker);
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

    // A search result card plays through the same path as the queue. (Auto-advance
    // for the Search source isn't wired yet — a finished search video just stops.)
    connect(searchModule, &Search::playMediaRequested, videoPlayer,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Search;
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
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

    // The skip buttons (single-click = nudge, double-click = jump tracks) land here.
    connect(videoPlayer, &VideoPlayer::skipRequested, this, [this](int delta) {
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
        });

    // Surface the player worker's logs (stream resolution, yt-dlp errors) in the
    // same dev console as the others, by re-emitting through the downloader.
    connect(playerWorker, &SgYtDlp::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

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

    // The startup check reports back here: install silently when auto-update is
    // on, otherwise ask with the themed prompt. Ignored while the first-run
    // setup dialog is driving the updater itself.
    connect(updaterWorker, &SgUpdater::checkFinished, this, [this](const QStringList& pending) {
        if (m_setupActive || pending.isEmpty()) return;
        QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
        if (cfg.value("General/AutoUpdate", true).toBool()) {
            QMetaObject::invokeMethod(updaterWorker, &SgUpdater::applyUpdates, Qt::QueuedConnection);
            return;
        }
        UpdateDialog dlg(updaterWorker, pending, mainWindow);
        dlg.exec();
        }, Qt::QueuedConnection);
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

void Seagull::run() {
    // The shell must be shown BEFORE the first-run dialog runs. Exec'ing an
    // application-modal dialog before any window was shown left the whole app
    // input-dead: deferred startup work (the player's winId()/VLC hookup)
    // fired inside the dialog's nested event loop, creating the native window
    // hierarchy underneath an active modal block.
    mainWindow->show();

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
        return;
    }

    // Defer the tool-update check until the UI has settled AND the Library's
    // thumbnail generation is idle, so update downloads never compete with the
    // startup ffmpeg work. Poll once a second after a short grace period.
    auto* gate = new QTimer(this);
    gate->setInterval(1000);
    connect(gate, &QTimer::timeout, this, [this, gate]() {
        if (libraryModule->thumbnailsBusy()) return;
        gate->stop();
        gate->deleteLater();
        QMetaObject::invokeMethod(updaterWorker, [w = updaterWorker]() { w->checkForUpdates(); },
            Qt::QueuedConnection);
        });
    QTimer::singleShot(3000, gate, [gate]() { gate->start(); });
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Apply the saved theme before any widgets are built so the whole UI is themed.
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    Theme::apply(settings.value("Display/Theme", "Seagull").toString());

    Seagull orchestrator;
    orchestrator.run();
    return app.exec();
}