#pragma once

#include <QDialog>
#include <QList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class SgUpdater;

// First-run welcome modal: shows the default save folders for confirmation or
// editing, explains that the external tools (yt-dlp / ffmpeg / Deno) will be
// downloaded if missing, and runs that download with live progress when the
// user accepts. Shown by the orchestrator modally over the freshly shown main
// window when isNeeded() — Setup/Completed unset, or any tool exe missing.
// (It must come after mainWindow->show(): exec'ing it before any window
// existed left the app input-dead.) Declining just
// closes it; the app still runs (local playback works) and asks again next
// launch because the flag stays unset.
class SetupDialog : public QDialog {
    Q_OBJECT

public:
    explicit SetupDialog(SgUpdater* updater, QWidget* parent = nullptr);

    static bool isNeeded();       // no Setup/Completed flag, or tools missing
    static bool toolsMissing();   // any of yt-dlp / ffmpeg+ffprobe / deno absent

private:
    void onGetStarted();          // save config -> download tools if needed -> accept
    void startToolDownload();     // progress mode; checkForUpdates -> applyUpdates

    SgUpdater* m_updater;

    QLineEdit* dlEdit;
    QLineEdit* videoEdit;
    QLineEdit* audioEdit;
    QLineEdit* photoEdit;
    QLineEdit* recEdit;

    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  startBtn;
    QPushButton*  skipBtn;
};
