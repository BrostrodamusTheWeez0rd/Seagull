#ifndef SEAGULL_H
#define SEAGULL_H

#include <QObject>
#include <QThread>
#include <QStringList>
#include "Modules/UI/MainWindow.h"
#include "Modules/UI/VideoPlayer.h"
#include "Modules/UI/Queue.h"
#include "Modules/UI/Library.h"
#include "Modules/UI/Search.h"
#include "Modules/UI/Settings.h"
#include "Modules/Backend/SgYtDlp.h"
#include "Modules/Backend/SgSearch.h"
#include "Modules/Backend/SgUpdater.h"
#include "Modules/Backend/SgHlsProxy.h"

class Seagull : public QObject {
    Q_OBJECT
public:
    explicit Seagull(QObject* parent = nullptr);
    ~Seagull();

    void run();

private:
    enum class ActiveSource { None, Library, Queue, Search };

    // Sequential downloader for ad-hoc downloads (e.g. Search cards). Files land in
    // the library; the Library tab shows a spinner while the queue drains.
    void pumpDownloads();

    MainWindow* mainWindow;
    VideoPlayer* videoPlayer;  // the playback feature, hosted by the shell window
    Queue* queueModule;
    Library* libraryModule;
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

    QStringList m_downloadQueue; // pending ad-hoc download URLs (FIFO)
    bool        m_downloading = false;

    // The tool updater does slow, blocking work (network fetches, hashing, unzip),
    // so it gets its own thread to keep startup and the UI snappy.
    SgUpdater* updaterWorker;
    QThread* updaterThread;
};

#endif