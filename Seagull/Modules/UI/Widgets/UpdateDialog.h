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
// Flow: checking (indeterminate bar) ->
//   - everything current: brief "Up to date" note, auto-closes
//   - updates found, AutoUpdate on: installs immediately with live progress
//   - updates found, AutoUpdate off: Update Now / Not Now prompt
// Escape and the (removed) close button are swallowed while busy; "Not Now"
// and the auto-closes are the only ways out. The updater lives on its own
// thread; calls go over queued invokes and its signals arrive queued.
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    UpdateDialog(SgUpdater* updater, bool autoInstall, QWidget* parent = nullptr);

protected:
    void reject() override; // swallowed while checking/installing

private:
    void onCheckFinished(const QStringList& pending);
    void startUpdate();   // prompt/auto -> progress state, kicks off applyUpdates
    void onProgress(const QString& tool, int percent);
    void onFinished(bool allOk);

    SgUpdater* m_updater;
    bool m_autoInstall;
    bool m_busy = true; // born checking; true again while installing

    QLabel*       titleLabel;
    QLabel*       bodyLabel;
    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  updateBtn;
    QPushButton*  laterBtn;
};
