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
#include "Modules/Backend/SgSpellCheck.h"
#include "Modules/Backend/SgUpdater.h"
#include "Modules/Backend/SgHlsProxy.h"
#include "Modules/Backend/SgRecorder.h"
#include "Modules/Backend/SgMediaControls.h"
#include "Modules/Backend/SgAppUpdate.h"

class QTextBrowser;
class SgThumbnailer;
class QTimer;
class QProgressDialog;

class Seagull : public QObject {
    Q_OBJECT
public:
    explicit Seagull(QObject* parent = nullptr);
    ~Seagull();

    bool run(); // false = the user declined the Terms of Use; main() should exit

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
    QTextBrowser* descriptionView; // the dynamic "Description" tab's page
    ActiveSource activeSource = ActiveSource::None;

    // The orchestrator owns every backend worker and hands them out to modules.
    SgYtDlp* downloaderWorker;
    SgYtDlp* resolverWorker;
    SgYtDlp* prefetcherWorker;
    SgYtDlp* playerWorker;     // dedicated to the player's probe/stream-url traffic
    SgYtDlp* downloadWorker;   // dedicated to ad-hoc (Search card) downloads
    SgSearch* searchWorker;    // backend for the Search tab (discovery)
    SgSpellCheck* spellChecker; // shared OS spell checker for the text fields

    // One shared localhost proxy that strips Twitch's stitched ad segments from the
    // live HLS manifest before VLC sees them. Handed to every resolve worker.
    SgHlsProxy* hlsProxy;

    // Records the currently-playing live stream to disk (parallel ffmpeg).
    SgRecorder* recorder;

    // Mirrors playback to the Windows media controls (SMTC) and routes the OS
    // media-key / overlay buttons back to the player. A timer pushes the timeline.
    SgMediaControls* mediaControls;
    QTimer* smtcTimelineTimer;
    void skipActive(int delta); // shared by the skip buttons + the SMTC next/prev keys

    // Checks GitHub Releases for a newer Seagull build and, if found, prompts the
    // user with the release notes + a button to open the download page.
    SgAppUpdate* appUpdate;
    bool m_appCheckManual = false;        // user pressed "Check for Updates" (Settings); show "up to date" too
    bool m_autoUpdateStartup = true;      // cached AutoUpdate setting for the startup flow
    bool m_selfUpdateFromStartup = false; // a startup-initiated self-update is downloading
    bool m_selfUpdateChosen = false;      // the startup UpdateDialog requested a Seagull self-update
    // The Settings manual "Check for Updates" prompt (rich notes + open-page). The
    // startup Seagull check is owned by the UpdateDialog instead.
    bool showAppUpdatePrompt(const QString& version, const QString& notes, const QString& pageUrl);

    // Startup update flow runs entirely inside run(): the two-stage UpdateDialog
    // (Seagull then tools) and, on first run, SetupDialog as the tool stage.
    void finishStartupUpdates(); // release thumbnail holds + shut the updater thread

    // Self-update: download + stage the new build (progress dialog), then launch a
    // helper that swaps the files once we exit and relaunches us.
    QProgressDialog* m_updateProgress = nullptr;
    void startSelfUpdate();
    void onUpdateReadyToApply(const QString& stagedAppDir);

    // Answers the player's local-file poster requests (frame grab / cover art).
    // Held with the Library's thumbnailer until the startup update modal is done.
    SgThumbnailer* playerThumbnailer;

    // Release the thumbnail ffmpeg queues once tool updates can no longer race them.
    void releaseThumbnailHolds();

    // The updater's only job is the startup flow; once that's done, stop its
    // thread instead of letting it idle for the whole session.
    void shutdownUpdater();

    QStringList m_downloadQueue; // pending ad-hoc download URLs (FIFO)
    bool        m_downloading = false;
    bool        m_setupActive = false; // first-run dialog owns the updater right now

    // The tool updater does slow, blocking work (network fetches, hashing, unzip),
    // so it gets its own thread to keep startup and the UI snappy.
    SgUpdater* updaterWorker;
    QThread* updaterThread;
};

#endif