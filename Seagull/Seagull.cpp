#include "Seagull.h"
#include <QApplication>
#include <QTimer>

Seagull::Seagull(QObject* parent) : QObject(parent) {
    // The window everything hangs off of.
    mainWindow = new MainWindow();

    // Backend workers. Each one is a yt-dlp wrapper with a single job, so their
    // long-running processes never step on each other.
    downloaderWorker = new SgYtDlp(this);
    resolverWorker = new SgYtDlp(this);
    prefetcherWorker = new SgYtDlp(this);
    playerWorker = new SgYtDlp(this);

    // The tab modules.
    libraryModule = new Library();
    downloadsModule = new Downloads(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search();
    settingsModule = new Settings();

    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(downloadsModule, "Downloads");
    mainWindow->addTab(searchModule, "Search");
    mainWindow->addTab(settingsModule, "Settings");

    // When a module wants something played, it tells the window. We remember which
    // source asked, so "play next" later knows whether to walk the library or the queue.
    connect(libraryModule, &Library::playMediaRequested, mainWindow, [this](const QUrl& url) {
        activeSource = ActiveSource::Library;
        mainWindow->playLocalFile(url);
        });

    connect(downloadsModule, &Downloads::playMediaRequested, mainWindow,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Downloads;
            mainWindow->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

    // A finished video rolls into the next one from whichever source is active.
    connect(mainWindow, &MainWindow::mediaEnded, this, [this]() {
        if (activeSource == ActiveSource::Library)        libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Downloads) downloadsModule->playNextQueuedItem();
        });

    // The skip buttons (single-click = nudge, double-click = jump tracks) land here.
    connect(mainWindow, &MainWindow::skipRequested, this, [this](int delta) {
        if (activeSource == ActiveSource::Library) {
            if (delta > 0) libraryModule->playNextFile();
            else           libraryModule->playPrevFile();
        }
        else if (activeSource == ActiveSource::Downloads) {
            if (delta > 0) downloadsModule->playNextQueuedItem();
            else           downloadsModule->playPrevQueuedItem();
        }
        });

    // Player asks the backend to resolve qualities and stream URLs on demand.
    connect(mainWindow, &MainWindow::probeQualitiesRequested, playerWorker, &SgYtDlp::probeAvailableQualities);

    connect(mainWindow, &MainWindow::streamUrlRequested, playerWorker, [this](const QString& url, const QString& formatId) {
        playerWorker->fetchMetadataAndStreamUrl(url, formatId);
        });

    // Results come back on queued connections so they always land on the UI thread.
    connect(playerWorker, &SgYtDlp::availableQualitiesFound, mainWindow, &MainWindow::handleAvailableQualities, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::streamUrlReady, mainWindow, &MainWindow::onStreamUrlReady, Qt::QueuedConnection);

    // --- Tool auto-update, off the main thread ---
    // Checking and downloading yt-dlp / Deno / ffmpeg means blocking network calls,
    // SHA-256 hashing, and unzipping that can take many seconds. Running that on the
    // UI thread froze startup, so the updater lives on its own thread instead. The
    // worker has no parent (a requirement for moveToThread); its child QProcess and
    // network manager move across with it automatically.
    updaterThread = new QThread(this);
    updaterWorker = new SgYtDlp(nullptr);
    updaterWorker->moveToThread(updaterThread);

    // Status lines still show up in the Downloads log, hopping back to the UI thread.
    connect(updaterWorker, &SgYtDlp::ytDlpUpdateStatus, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // Tear the worker down with its thread.
    connect(updaterThread, &QThread::finished, updaterWorker, &QObject::deleteLater);

    updaterThread->start();

    // Give the UI a few seconds to settle, then kick the update check off on the
    // worker thread (QueuedConnection makes sure it runs there, not here).
    QTimer::singleShot(3000, this, [this]() {
        QMetaObject::invokeMethod(updaterWorker, &SgYtDlp::checkForYtDlpUpdate, Qt::QueuedConnection);
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

void Seagull::run() {
    mainWindow->show();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    Seagull orchestrator;
    orchestrator.run();
    return app.exec();
}