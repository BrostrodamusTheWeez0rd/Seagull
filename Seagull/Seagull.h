#ifndef SEAGULL_H
#define SEAGULL_H

#include <QObject>
#include <QThread>
#include <QStringList>
#include "Modules/UI/MainWindow.h"
#include "Modules/UI/VideoPlayer.h"
#include "Modules/UI/Queue.h"
#include "Modules/UI/FileExplorer.h"
#include "Modules/UI/MediaLibrary.h"
#include "Modules/UI/Search.h"
#include "Modules/UI/Settings.h"
#include "Modules/Backend/SgYtDlp.h"
#include "Modules/Backend/SgSearch.h"
#include "Modules/Backend/SgUpdater.h"
#include "Modules/Backend/SgHlsProxy.h"
#include "Modules/Backend/SgRecorder.h"

class Seagull : public QObject {
    Q_OBJECT
public:
    explicit Seagull(QObject* parent = nullptr);
    ~Seagull();

    void run();

private:
    // Library = the card-grid media library; Explorer = the file-manager tab.
    enum class ActiveSource { None, Library, Explorer, Queue, Search };

    // Sequential downloader for ad-hoc downloads (e.g. Search cards). Files land in
    // the library; the Library tab shows a spinner while the queue drains.
    void pumpDownloads();

    // Brief seagull on the Library tab once a recording/clip is saved + playable.
    void flashLibraryTab();

    MainWindow* mainWindow;
    VideoPlayer* videoPlayer;  // the playback feature, hosted by the shell window
    Queue* queueModule;
    MediaLibrary* libraryModule;   // card-grid view of the saved-media folders
    FileExplorer* explorerModule;  // the file-manager tab (was "Library")
    Search* searchModule;
    Settings* settingsModule;
    ActiveSource activeSource = ActiveSource::None;

    // The orchestrator owns every backend worker and hands them out to modules.
    SgYtDlp* downloaderWorker;
    SgYtDlp* resolverWorker;
    SgYtDlp* prefetcherWorker;
    SgYtDlp* playerWorker;     // dedicated to the player's probe/stream-url traffic
    SgYtDlp* downloadWorker;   // dedicated to ad-hoc (Search card) downloads
    SgSearch* searchWorker;    // backend for the Search tab (discovery)

    // One shared localhost proxy that strips Twitch's stitched ad segments from the
    // live HLS manifest before VLC sees them. Handed to every resolve worker.
    SgHlsProxy* hlsProxy;

    // Records the currently-playing live stream to disk (parallel ffmpeg).
    SgRecorder* recorder;

    QStringList m_downloadQueue; // pending ad-hoc download URLs (FIFO)
    bool        m_downloading = false;

    // The tool updater does slow, blocking work (network fetches, hashing, unzip),
    // so it gets its own thread to keep startup and the UI snappy.
    SgUpdater* updaterWorker;
    QThread* updaterThread;
};

#endif