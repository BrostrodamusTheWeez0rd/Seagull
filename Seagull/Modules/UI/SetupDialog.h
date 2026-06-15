#pragma once

#include <QDialog>
#include <QList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QCheckBox;
class SgUpdater;

// First-run welcome modal: shows the default save folders for confirmation or
// editing, explains that the external tools (yt-dlp / ffmpeg / Deno) will be
// downloaded if missing, and runs that download with live progress when the
// user accepts. Shown by the orchestrator modally when isNeeded() —
// Setup/Completed unset, or any tool exe missing. It runs as stage 2 of the
// startup flow, BEFORE the main window is shown (the input-dead landmine that
// once forced it after mainWindow->show() is gone now that the player no longer
// queues a deferred winId()/VLC hookup — see Seagull::run). Declining just
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
    QLineEdit* playlistEdit;

    QCheckBox* desktopShortcutCheck;
    QCheckBox* startMenuShortcutCheck;

    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  startBtn;
    QPushButton*  skipBtn;
};
