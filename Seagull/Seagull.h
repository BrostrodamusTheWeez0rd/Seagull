#ifndef SEAGULL_H
#define SEAGULL_H

#include <QObject>
#include "Modules/UI/MainWindow.h"
#include "Modules/UI/Downloads.h"
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
    enum class ActiveSource { None, Library, Downloads };

    MainWindow* mainWindow;
    Downloads* downloadsModule;
    Library* libraryModule;
    Search* searchModule;
    Settings* settingsModule;
    ActiveSource activeSource = ActiveSource::None;

    // Orchestrator now owns the backend workers
    SgYtDlp* downloaderWorker;
    SgYtDlp* resolverWorker;
    SgYtDlp* prefetcherWorker;
};

#endif