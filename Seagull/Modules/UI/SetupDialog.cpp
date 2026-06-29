#include "SetupDialog.h"
#include "../Backend/SgPaths.h"
#include "../Backend/SgUpdater.h"
#include "../Backend/SgMediaControls.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFont>

bool SetupDialog::toolsMissing() {
    const QString tools = QCoreApplication::applicationDirPath() + "/tools";
    return !QFile::exists(tools + "/yt-dlp.exe")
        || !QFile::exists(tools + "/ffmpeg.exe")
        || !QFile::exists(tools + "/ffprobe.exe")
        || !QFile::exists(tools + "/deno.exe");
}

bool SetupDialog::isNeeded() {
    // Setup is the FIRST-RUN experience only — folder confirmation, shortcuts,
    // the Defender offer, and the initial tool download — so it is gated purely
    // on the Setup/Completed flag. Missing or outdated tools on an already
    // set-up install are NOT a setup concern: the startup update stage (and the
    // Settings "Check for Updates" button) detect and download them through the
    // small update dialog, which already handles "not installed -> download" for
    // every managed tool. Folding toolsMissing() in here was the bug — a newly
    // added tool (e.g. AtomicParsley) threw existing users back into the full
    // setup flow on every launch instead of just quietly fetching it.
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    if (cfg.value("Setup/Completed", false).toBool())
        return false;

    // Migration safety-net: a config that predates the Setup/Completed flag (a
    // pre-v0.9.0 build, or a hand-edited/partially-reset config) is still an
    // EXISTING install, not a first run — don't drag those users back through
    // setup. A [Paths] section is written only by completing setup or changing a
    // folder in Settings; the Terms modal that runs before this check writes only
    // Setup/*, and SgPaths reads never persist folder keys, so its presence is a
    // reliable "already set up" signal. Adopt them: stamp the flag (one-time) so
    // the missing tool gets fetched silently by the update stage, not via setup.
    if (cfg.childGroups().contains("Paths")) {
        cfg.setValue("Setup/Completed", true);
        return false;
    }

    return true; // genuine first run — no prior config to inherit
}

SetupDialog::SetupDialog(SgUpdater* updater, QWidget* parent)
    : QDialog(parent), m_updater(updater) {
    setWindowTitle("Welcome to Seagull");
    setModal(true);
    setMinimumWidth(520);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(12);

    auto* title = new QLabel("Welcome to Seagull", this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() + 4);
    tf.setBold(true);
    title->setFont(tf);
    lay->addWidget(title);

    auto* blurb = new QLabel(
        "Here's where Seagull will keep your media. Confirm the folders below or "
        "pick your own. Everything can be changed later in Settings.", this);
    blurb->setWordWrap(true);
    lay->addWidget(blurb);

    // Folder rows, prefilled with the live defaults (the user's Windows folders
    // on a fresh install — SgPaths owns that logic).
    auto* form = new QFormLayout();
    form->setHorizontalSpacing(12);
    auto addFolderRow = [this, form](QLineEdit*& edit, const QString& label,
                                     const QString& path, const QString& title) {
        auto* row = new QHBoxLayout();
        edit = new QLineEdit(path);
        edit->setReadOnly(true);
        auto* btn = new QPushButton("Browse...");
        row->addWidget(edit);
        row->addWidget(btn);
        QLineEdit* target = edit;
        connect(btn, &QPushButton::clicked, this, [this, target, title]() {
            const QString dir = QFileDialog::getExistingDirectory(this, title, target->text());
            if (!dir.isEmpty()) target->setText(dir);
            });
        form->addRow(label, row);
        };
    addFolderRow(dlEdit,       "Downloads:",  SgPaths::downloadFolder(),       "Select Downloads Folder");
    addFolderRow(videoEdit,    "Videos:",     SgPaths::videoFolder(false),     "Select Videos Folder");
    addFolderRow(audioEdit,    "Audio:",      SgPaths::audioFolder(false),     "Select Audio Folder");
    addFolderRow(photoEdit,    "Photos:",     SgPaths::photoFolder(false),     "Select Photos Folder");
    addFolderRow(recEdit,      "Recordings:", SgPaths::recordingFolder(false), "Select Recordings Folder");
    addFolderRow(playlistEdit, "Playlists:",  SgPaths::playlistFolder(false),  "Select Playlists Folder");
    lay->addLayout(form);

    if (toolsMissing()) {
        auto* depNote = new QLabel(
            "Seagull also needs a few free tools to stream, download and record: "
            "yt-dlp, ffmpeg, Deno and AtomicParsley (about 150 MB). Seagull will "
            "not work without them.", this);
        depNote->setWordWrap(true);
        depNote->setObjectName("metaStats"); // theme's dimmed text styling
        lay->addWidget(depNote);
    }

    // Shortcuts (default on). The Start-menu one also gives Windows our app
    // identity, so the media controls card shows "Seagull" instead of "unknown app".
    desktopShortcutCheck = new QCheckBox("Add a desktop shortcut", this);
    desktopShortcutCheck->setChecked(true);
    lay->addWidget(desktopShortcutCheck);
    startMenuShortcutCheck = new QCheckBox("Add a Start menu shortcut", this);
    startMenuShortcutCheck->setChecked(true);
    lay->addWidget(startMenuShortcutCheck);

    // Defender exclusion (recommended). After a reboot or a long idle, Windows
    // Defender rescans Seagull's many DLLs and VLC plugins on launch, which is the
    // main reason the first start can be slow before later ones are instant. The
    // exclusion needs admin, so accepting raises a UAC prompt (see onGetStarted).
    defenderExclusionCheck = new QCheckBox(
        "Speed up startup by adding Seagull to Windows Defender's exclusions (recommended)", this);
    defenderExclusionCheck->setChecked(true);
    lay->addWidget(defenderExclusionCheck);
    auto* defenderNote = new QLabel(
        "Windows Defender rescans Seagull's files after every restart, which can make "
        "the first launch slow. Excluding the app folder lets Seagull start quickly "
        "every time. Windows will ask for permission.", this);
    defenderNote->setWordWrap(true);
    defenderNote->setObjectName("metaStats"); // theme's dimmed text styling
    lay->addWidget(defenderNote);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("metaStats");
    statusLabel->hide();
    lay->addWidget(statusLabel);

    progressBar = new QProgressBar(this); // themed by the global sheet
    progressBar->setRange(0, 100);
    progressBar->hide();
    lay->addWidget(progressBar);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    skipBtn = new QPushButton("Not Now", this);
    // "Download" when the tools still need fetching, so it's clear what the
    // button does; plain "Get Started" when it's only confirming folders.
    startBtn = new QPushButton(toolsMissing() ? "Download" : "Get Started", this);
    startBtn->setDefault(true);
    btnRow->addWidget(skipBtn);
    btnRow->addWidget(startBtn);
    lay->addLayout(btnRow);

    connect(skipBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(startBtn, &QPushButton::clicked, this, &SetupDialog::onGetStarted);
}

void SetupDialog::onGetStarted() {
    // Persist the confirmed folders + the done flag. SgPaths reads these keys,
    // so every module picks them up with no further plumbing.
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    cfg.setValue("Paths/DownloadFolder", dlEdit->text());
    cfg.setValue("Paths/VideoFolder", videoEdit->text());
    cfg.setValue("Paths/AudioFolder", audioEdit->text());
    cfg.setValue("Paths/PhotoFolder", photoEdit->text());
    cfg.setValue("Paths/RecordingFolder", recEdit->text());
    cfg.setValue("Paths/PlaylistFolder", playlistEdit->text());
    cfg.setValue("Setup/Completed", true);
    // The Defender choice was offered here (the checkbox below), so the one-time
    // post-update prompt in Seagull::run() should never fire for a fresh install.
    cfg.setValue("Setup/DefenderExclusionOffered", true);
    cfg.sync();

    // The Windows folders already exist, but the defaults that live in a
    // subfolder (Videos\Recordings, Documents\Playlists) don't yet — create
    // every confirmed folder so the Library tabs never point at nothing.
    for (const QLineEdit* e : { dlEdit, videoEdit, audioEdit, photoEdit, recEdit, playlistEdit })
        if (!e->text().isEmpty()) QDir().mkpath(e->text());

    // Create the requested shortcuts (best-effort; never blocks setup).
    if (desktopShortcutCheck->isChecked())   SgMediaControls::createDesktopShortcut();
    if (startMenuShortcutCheck->isChecked()) SgMediaControls::createStartMenuShortcut();

    // Best-effort; raises a UAC prompt. Fire it before the tool download so the
    // consent prompt isn't sprung mid-progress. Declining changes nothing. If the
    // add ran but didn't stick (Tamper Protection), tell the user how to finish by
    // hand so they're not left thinking startup is sped up when it isn't.
    if (defenderExclusionCheck->isChecked()) {
        const SgMediaControls::DefenderResult result = SgMediaControls::addDefenderExclusion();
        if (result == SgMediaControls::DefenderResult::Success) {
            cfg.setValue("Setup/DefenderExcluded", true); // Settings reads this back (non-elevated can't)
            cfg.sync();
        }
        const QString message = SgMediaControls::defenderResultMessage(result);
        if (!message.isEmpty()) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle("Defender Exclusion");
            box.setText(message);
            QPushButton* openBtn = box.addButton("Open Windows Security", QMessageBox::ActionRole);
            box.addButton(QMessageBox::Close);
            box.exec();
            if (box.clickedButton() == openBtn) SgMediaControls::openDefenderSettings();
        }
    }

    if (toolsMissing()) startToolDownload();
    else accept();
}

void SetupDialog::startToolDownload() {
    startBtn->setEnabled(false);
    skipBtn->setEnabled(false);
    statusLabel->setText("Checking tools...");
    statusLabel->show();
    progressBar->show();

    // check (fills the updater's pending queue, incl. the missing tools) then
    // apply. Both hop to the updater's thread; results arrive queued.
    connect(m_updater, &SgUpdater::checkFinished, this, [this](const QStringList& pending) {
        if (pending.isEmpty()) { accept(); return; } // nothing to fetch after all
        QMetaObject::invokeMethod(m_updater, &SgUpdater::applyUpdates, Qt::QueuedConnection);
        });
    connect(m_updater, &SgUpdater::applyProgress, this, [this](const QString& tool, int percent) {
        statusLabel->setText("Downloading " + tool + "...");
        progressBar->setValue(percent);
        });
    connect(m_updater, &SgUpdater::applyFinished, this, [this](bool allOk) {
        progressBar->setValue(100);
        if (allOk) { accept(); return; }
        statusLabel->setText("Some tools could not be downloaded. Seagull will retry next launch.");
        skipBtn->hide();
        startBtn->setText("Close");
        startBtn->setEnabled(true);
        disconnect(startBtn, &QPushButton::clicked, this, &SetupDialog::onGetStarted);
        connect(startBtn, &QPushButton::clicked, this, &QDialog::accept);
        });

    QMetaObject::invokeMethod(m_updater, [u = m_updater]() { u->checkForUpdates(true); },
        Qt::QueuedConnection);
}
