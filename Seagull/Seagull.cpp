#include "Seagull.h"
#include "Modules/UI/Theme.h"
#include <QApplication>
#include <QSettings>
#include <QCoreApplication>
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

    // The tab modules.
    libraryModule = new Library();
    queueModule = new Queue(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search(searchWorker);
    settingsModule = new Settings();

    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(queueModule, "Queue");
    mainWindow->addTab(searchModule, "Search");
    mainWindow->addTab(settingsModule, "Settings");

    // When a module wants something played, it tells the window. We remember which
    // source asked, so "play next" later knows whether to walk the library or the queue.
    connect(libraryModule, &Library::playMediaRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Library;
        videoPlayer->playLocalFile(url);
        });

    connect(queueModule, &Queue::playMediaRequested, videoPlayer,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Queue;
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

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

    // Display "Card size" resizes the Search result cards live.
    connect(settingsModule, &Settings::cardWidthChanged, searchModule, &Search::setCardWidth);

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

    // A finished video rolls into the next one from whichever source is active.
    connect(videoPlayer, &VideoPlayer::mediaEnded, this, [this]() {
        if (activeSource == ActiveSource::Library)        libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Queue) queueModule->playNextQueuedItem();
        });

    // The skip buttons (single-click = nudge, double-click = jump tracks) land here.
    connect(videoPlayer, &VideoPlayer::skipRequested, this, [this](int delta) {
        if (activeSource == ActiveSource::Library) {
            if (delta > 0) libraryModule->playNextFile();
            else           libraryModule->playPrevFile();
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

    connect(videoPlayer, &VideoPlayer::streamUrlRequested, playerWorker, [this](const QString& url, const QString& formatId) {
        playerWorker->cancel(); // free the worker (e.g. drop an in-flight quality probe) so the resolve runs now
        playerWorker->fetchMetadataAndStreamUrl(url, formatId);
        });

    // Results come back on queued connections so they always land on the UI thread.
    connect(playerWorker, &SgYtDlp::availableQualitiesFound, videoPlayer, &VideoPlayer::handleAvailableQualities, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::liveStatusKnown, videoPlayer, &VideoPlayer::onLiveStatus, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::thumbnailResolved, videoPlayer, &VideoPlayer::onThumbnailResolved, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::videoInfoReady, videoPlayer, &VideoPlayer::onVideoInfo, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::streamUrlReady, videoPlayer, &VideoPlayer::onStreamUrlReady, Qt::QueuedConnection);

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

    // Give the UI a few seconds to settle, then kick the update check off on the
    // worker thread (QueuedConnection makes sure it runs there, not here).
    QTimer::singleShot(3000, this, [this]() {
        QMetaObject::invokeMethod(updaterWorker, &SgUpdater::checkForUpdates, Qt::QueuedConnection);
        });
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

void Seagull::run() {
    mainWindow->show();
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