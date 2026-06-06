#include "Seagull.h"
#include <QApplication>
#include <QTimer>

Seagull::Seagull(QObject* parent) : QObject(parent) {
    // 1. Instantiate the UI Shell
    mainWindow = new MainWindow();

    // 2. Instantiate the three backend workers for the Downloads module
    downloaderWorker = new SgYtDlp(this);
    resolverWorker = new SgYtDlp(this);
    prefetcherWorker = new SgYtDlp(this);

    // 3. Instantiate modules, injecting the backend workers into Downloads
    libraryModule = new Library();
    downloadsModule = new Downloads(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search();
    settingsModule = new Settings();

    // 4. Inject modules into the shell's tab system
    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(downloadsModule, "Downloads");
    mainWindow->addTab(searchModule, "Search");
    mainWindow->addTab(settingsModule, "Settings");

    // 5. Orchestrate playback routing
    connect(libraryModule, &Library::playMediaRequested, mainWindow, [this](const QUrl& url) {
        activeSource = ActiveSource::Library;
        mainWindow->playLocalFile(url);
        });

    connect(downloadsModule, &Downloads::playMediaRequested, mainWindow,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Downloads;
            mainWindow->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

    // 6. Cross-module events
    connect(mainWindow, &MainWindow::mediaEnded, this, [this]() {
        if (activeSource == ActiveSource::Library)        libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Downloads) downloadsModule->playNextQueuedItem();
        });
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

    // 7. Backend resolution routing
    // FIX: Probe is now routed to resolverWorker so it doesn't collide with the downloader
    connect(mainWindow, &MainWindow::probeQualitiesRequested, resolverWorker, &SgYtDlp::probeAvailableQualities);

    connect(mainWindow, &MainWindow::streamUrlRequested, downloaderWorker, [this](const QString& url, const QString& formatId) {
        downloaderWorker->fetchMetadataAndStreamUrl(url, formatId);
        });

    // Feed backend results back to the UI shell
    // FIX: Add connection for the resolver worker to update the UI
    connect(resolverWorker, &SgYtDlp::availableQualitiesFound, mainWindow, &MainWindow::handleAvailableQualities, Qt::QueuedConnection);
    connect(downloaderWorker, &SgYtDlp::availableQualitiesFound, mainWindow, &MainWindow::handleAvailableQualities, Qt::QueuedConnection);

    // REMOVED: Global signal leak removed below
    // connect(downloaderWorker, &SgYtDlp::streamUrlReady, mainWindow, &MainWindow::onStreamUrlReady);

    // yt-dlp auto-update on startup — routed through logMessage so Downloads log shows it
    connect(downloaderWorker, &SgYtDlp::ytDlpUpdateStatus, downloaderWorker, &SgYtDlp::logMessage);
    QTimer::singleShot(3000, downloaderWorker, &SgYtDlp::checkForYtDlpUpdate);
}

Seagull::~Seagull() {
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