#include "Seagull.h"
#include <QApplication>
#include <QTimer>

Seagull::Seagull(QObject* parent) : QObject(parent) {
    // 1. Instantiate the UI Shell
    mainWindow = new MainWindow();

    // 2. Instantiate the backend workers
    downloaderWorker = new SgYtDlp(this);
    resolverWorker = new SgYtDlp(this);
    prefetcherWorker = new SgYtDlp(this);
    playerWorker = new SgYtDlp(this); // Now a member property

    // 3. Instantiate modules
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

    // 6. Cross-module playback sequence events
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

    // 7. Player-exclusive backend resolution routing (The Orchestrator Path)
    connect(mainWindow, &MainWindow::probeQualitiesRequested, playerWorker, &SgYtDlp::probeAvailableQualities);

    connect(mainWindow, &MainWindow::streamUrlRequested, playerWorker, [this](const QString& url, const QString& formatId) {
        playerWorker->fetchMetadataAndStreamUrl(url, formatId);
        });

    // Safely return results to the UI Shell
    connect(playerWorker, &SgYtDlp::availableQualitiesFound, mainWindow, &MainWindow::handleAvailableQualities, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::streamUrlReady, mainWindow, &MainWindow::onStreamUrlReady, Qt::QueuedConnection);

    // 8. yt-dlp auto-update
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