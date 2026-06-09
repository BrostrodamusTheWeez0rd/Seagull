#ifndef SEAGULL_H
#define SEAGULL_H

#include <QObject>
#include <QThread>
#include "Modules/UI/MainWindow.h"
#include "Modules/UI/VideoPlayer.h"
#include "Modules/UI/Queue.h"
#include "Modules/UI/Library.h"
#include "Modules/UI/Search.h"
#include "Modules/UI/Settings.h"
#include "Modules/Backend/SgYtDlp.h"

class Seagull : public QObject {
    Q_OBJECT
public:
    explicit Seagull(QObject* parent = nullptr);
    ~Seagull();

    void run();

private:
    enum class ActiveSource { None, Library, Queue };

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

    // The tool updater does slow, blocking work (network fetches, hashing, unzip),
    // so it gets its own thread to keep startup and the UI snappy.
    SgYtDlp* updaterWorker;
    QThread* updaterThread;
};

#endif