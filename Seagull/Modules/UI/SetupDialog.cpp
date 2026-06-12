#include "SetupDialog.h"
#include "../Backend/SgPaths.h"
#include "../Backend/SgUpdater.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
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
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    return !cfg.value("Setup/Completed", false).toBool() || toolsMissing();
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
            "yt-dlp, ffmpeg and Deno (about 150 MB). Seagull will not work "
            "without them.", this);
        depNote->setWordWrap(true);
        depNote->setObjectName("metaStats"); // theme's dimmed text styling
        lay->addWidget(depNote);
    }

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
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    cfg.setValue("Paths/DownloadFolder", dlEdit->text());
    cfg.setValue("Paths/VideoFolder", videoEdit->text());
    cfg.setValue("Paths/AudioFolder", audioEdit->text());
    cfg.setValue("Paths/PhotoFolder", photoEdit->text());
    cfg.setValue("Paths/RecordingFolder", recEdit->text());
    cfg.setValue("Paths/PlaylistFolder", playlistEdit->text());
    cfg.setValue("Setup/Completed", true);
    cfg.sync();

    // The Windows folders already exist, but the defaults that live in a
    // subfolder (Videos\Recordings, Documents\Playlists) don't yet — create
    // every confirmed folder so the Library tabs never point at nothing.
    for (const QLineEdit* e : { dlEdit, videoEdit, audioEdit, photoEdit, recEdit, playlistEdit })
        if (!e->text().isEmpty()) QDir().mkpath(e->text());

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
