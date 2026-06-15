#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QProgressBar;
class QPushButton;
class SgUpdater;

// Startup update modal. Locks the app from the version check through any
// installs, so a tool can never be replaced while something might spawn it
// (thumbnails, searches, downloads, stream resolution).
//
// AutoUpdate on:  opens straight into the check (indeterminate bar), then
//                 installs immediately with live progress.
// AutoUpdate off: opens on an ask-first stage ("Check for updates?"); nothing
//                 touches the network until the user says Check Now, and any
//                 updates found get a second Update Now / Not Now prompt.
// Either way: "Up to date" and successful installs auto-close; Escape and the
// (removed) titlebar close are swallowed while checking/installing. The
// updater lives on its own thread; calls go over queued invokes and its
// signals arrive queued.
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    UpdateDialog(SgUpdater* updater, bool autoInstall, QWidget* parent = nullptr);

    // True if a check actually ran (auto, or the user clicked Check Now). Lets the
    // app-version check respect the same decision instead of re-prompting.
    bool ranCheck() const { return m_checkStarted; }

protected:
    void reject() override; // swallowed while checking/installing

private:
    enum class Stage { Ask, Checking, Prompt, Installing, Done };

    void enterAskStage();    // AutoUpdate off: offer the check before running it
    void beginCheck();       // checking UI + kicks checkForUpdates on the worker
    void startUpdate();      // installing UI + kicks applyUpdates on the worker
    void onPrimaryClicked(); // the right-hand button, by stage
    void onCheckFinished(const QStringList& pending);
    void onProgress(const QString& tool, int percent);
    void onFinished(bool allOk);

    SgUpdater* m_updater;
    bool  m_autoInstall;
    bool  m_busy = false;            // true while checking/installing (locks reject)
    bool  m_checkStarted = false;    // a version check actually ran (see ranCheck)
    Stage m_stage = Stage::Checking;

    QLabel*       titleLabel;
    QLabel*       bodyLabel;
    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  updateBtn; // primary: Check Now / Update Now / Close, by stage
    QPushButton*  laterBtn;
};
