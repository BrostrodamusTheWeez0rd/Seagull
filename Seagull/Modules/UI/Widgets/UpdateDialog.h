#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class SgUpdater;
class SgAppUpdate;

// Startup update modal. Shown BEFORE the main window (the window stays hidden
// until it closes), and locks itself from the version check through any installs
// so a tool can never be replaced while something might spawn it (thumbnails,
// searches, downloads, stream resolution).
//
// Two real stages in one modal:
//   1. "Checking for a new version of Seagull" (SgAppUpdate). A found app update
//      always prompts (Update Now / Not Now); Update Now emits selfUpdateRequested
//      and closes so the orchestrator drives the download/swap/relaunch. Not Now
//      (or up-to-date) falls through to stage 2.
//   2. "Checking tools (yt-dlp, ffmpeg, Deno, AtomicParsley)" (SgUpdater) — but only when
//      runToolStage is true. On first run the orchestrator owns the tool stage via
//      SetupDialog, so the dialog closes after stage 1.
//
// AutoUpdate on:  opens straight into stage 1, then auto-installs tools.
// AutoUpdate off: opens on an ask-first stage ("Check for updates?"); nothing
//                 touches the network until the user says Check Now, and any tool
//                 updates found get a second Update Now / Not Now prompt.
// Either way: "Up to date" and successful installs auto-close; Escape and the
// (removed) titlebar close are swallowed while checking/installing. The tool
// updater lives on its own thread; calls go over queued invokes and its signals
// arrive queued.
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    // skipAsk: start checking immediately even when autoInstall is off (used when
    // the caller already asked the user's permission to check). runToolStage:
    // run the tool check after the Seagull check (false on first run, where Setup
    // is the tool stage). Tools are still confirmed before install when
    // autoInstall is off.
    UpdateDialog(SgAppUpdate* appUpdate, SgUpdater* toolUpdater,
                 bool autoInstall, bool skipAsk = false,
                 bool runToolStage = true, QWidget* parent = nullptr);

signals:
    // The user accepted a Seagull update at stage 1; the orchestrator downloads,
    // swaps and relaunches (it owns the self-update machinery). The dialog closes.
    void selfUpdateRequested();

protected:
    void reject() override; // swallowed while checking/installing

private:
    enum class Stage { Ask, AppChecking, AppPrompt, Checking, Prompt, Installing, Done };

    void enterAskStage();    // AutoUpdate off: offer the check before running it
    void beginAppCheck();    // stage 1 UI + kicks SgAppUpdate::checkForUpdate
    void proceedToTools();   // stage 1 done -> tool stage, or close on first run
    void beginCheck();       // stage 2 UI + kicks checkForUpdates on the tool worker
    void startUpdate();      // installing UI + kicks applyUpdates on the tool worker
    void onPrimaryClicked();  // the right-hand button, by stage
    void onAppUpToDate();
    void onAppUpdateAvailable(const QString& version, const QString& notes, const QString& pageUrl);
    void onAppCheckFailed(const QString& reason);
    void onCheckFinished(const QStringList& pending);
    void onProgress(const QString& tool, int percent);
    void onFinished(bool allOk);

    SgAppUpdate* m_appUpdate;
    SgUpdater* m_updater;
    bool  m_autoInstall;
    bool  m_skipAsk;
    bool  m_runToolStage;
    bool  m_busy = false;            // true while checking/installing (locks reject)
    Stage m_stage = Stage::AppChecking;

    QLabel*       titleLabel;
    QLabel*       bodyLabel;
    QTextBrowser* notesView;  // scrollable, markdown-rendered release notes (app-update prompt only)
    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  updateBtn; // primary: Check Now / Update Now / Close, by stage
    QPushButton*  laterBtn;
};
