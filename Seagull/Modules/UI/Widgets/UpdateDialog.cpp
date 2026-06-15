#include "UpdateDialog.h"
#include "../../Backend/SgUpdater.h"
#include "../../Backend/SgAppUpdate.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QFont>

UpdateDialog::UpdateDialog(SgAppUpdate* appUpdate, SgUpdater* toolUpdater,
                           bool autoInstall, bool skipAsk, bool runToolStage, QWidget* parent)
    : QDialog(parent), m_appUpdate(appUpdate), m_updater(toolUpdater),
      m_autoInstall(autoInstall), m_skipAsk(skipAsk), m_runToolStage(runToolStage) {
    setWindowTitle("Seagull Updater");
    setModal(true);
    setMinimumWidth(440);
    // No titlebar close button: the lock is the point. reject() swallows Escape.
    setWindowFlags((windowFlags() | Qt::CustomizeWindowHint)
                   & ~Qt::WindowCloseButtonHint & ~Qt::WindowContextHelpButtonHint);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(12);

    titleLabel = new QLabel(this);
    QFont tf = titleLabel->font();
    tf.setPointSizeF(tf.pointSizeF() + 3);
    tf.setBold(true);
    titleLabel->setFont(tf);
    lay->addWidget(titleLabel);

    bodyLabel = new QLabel(this);
    bodyLabel->setTextFormat(Qt::PlainText);
    bodyLabel->setWordWrap(true);
    lay->addWidget(bodyLabel);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("metaStats"); // theme's dimmed text styling
    lay->addWidget(statusLabel);

    progressBar = new QProgressBar(this); // themed by the global sheet
    lay->addWidget(progressBar);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    laterBtn = new QPushButton("Not Now", this);
    updateBtn = new QPushButton(this);
    updateBtn->setDefault(true);
    btnRow->addWidget(laterBtn);
    btnRow->addWidget(updateBtn);
    lay->addLayout(btnRow);

    connect(laterBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(updateBtn, &QPushButton::clicked, this, &UpdateDialog::onPrimaryClicked);

    // Stage 1 (Seagull) signals. SgAppUpdate runs on the GUI thread (QNAM), so
    // these arrive directly. The dialog only exists during startup, so it never
    // collides with the Settings "Check for Updates" path.
    connect(m_appUpdate, &SgAppUpdate::updateAvailable, this, &UpdateDialog::onAppUpdateAvailable);
    connect(m_appUpdate, &SgAppUpdate::upToDate,        this, &UpdateDialog::onAppUpToDate);
    connect(m_appUpdate, &SgAppUpdate::checkFailed,     this, &UpdateDialog::onAppCheckFailed);

    // Stage 2 (tools) signals. The tool updater emits from its own thread, queued.
    connect(m_updater, &SgUpdater::checkFinished, this, &UpdateDialog::onCheckFinished);
    connect(m_updater, &SgUpdater::applyProgress, this, &UpdateDialog::onProgress);
    connect(m_updater, &SgUpdater::applyFinished, this, &UpdateDialog::onFinished);

    // Always offer the ask first, unless the caller explicitly skips it. autoInstall
    // governs only whether the tool stage installs without a second prompt (after
    // the user already said yes here) — it must NOT bypass this question, so a
    // startup check never runs without the user's go-ahead.
    if (m_skipAsk) beginAppCheck();
    else           enterAskStage();
}

void UpdateDialog::reject() {
    if (m_busy) return; // locked while checking/installing
    QDialog::reject();
}

void UpdateDialog::onPrimaryClicked() {
    switch (m_stage) {
    case Stage::Ask:       beginAppCheck();         break;
    case Stage::AppPrompt: emit selfUpdateRequested(); accept(); break;
    case Stage::Prompt:    startUpdate();           break;
    case Stage::Done:      accept();                break;
    default: break; // busy stages have no primary button showing
    }
}

void UpdateDialog::enterAskStage() {
    m_stage = Stage::Ask;
    m_busy  = false;
    titleLabel->setText("Check for updates?");
    bodyLabel->setText(
        "Look for a new version of Seagull and its tools (yt-dlp, ffmpeg, Deno) now?\n"
        "Seagull stays locked while a check or install runs, so a tool is "
        "never replaced mid-use.");
    statusLabel->hide();
    progressBar->hide();
    updateBtn->setText("Check Now");
    updateBtn->show();
    laterBtn->show();
}

// ---- Stage 1: Seagull ------------------------------------------------------

void UpdateDialog::beginAppCheck() {
    m_stage = Stage::AppChecking;
    m_busy  = true;
    titleLabel->setText("Checking for a new version of Seagull");
    bodyLabel->setText("Looking for a newer Seagull release.");
    statusLabel->setText("Contacting update servers...");
    statusLabel->show();
    progressBar->setRange(0, 0); // indeterminate
    progressBar->show();
    updateBtn->hide();
    laterBtn->hide();

    m_appUpdate->checkForUpdate();
}

void UpdateDialog::onAppUpToDate() {
    if (m_stage != Stage::AppChecking) return;
    proceedToTools();
}

void UpdateDialog::onAppCheckFailed(const QString& /*reason*/) {
    // A failed Seagull check must not block the tools; carry on quietly.
    if (m_stage != Stage::AppChecking) return;
    proceedToTools();
}

void UpdateDialog::onAppUpdateAvailable(const QString& version, const QString& notes,
                                        const QString& /*pageUrl*/) {
    if (m_stage != Stage::AppChecking) return;
    m_stage = Stage::AppPrompt;
    m_busy  = false;
    titleLabel->setText(QString("Seagull %1 is available").arg(version));
    bodyLabel->setText(notes.trimmed().isEmpty()
        ? QStringLiteral("A new version of Seagull is available.")
        : notes.trimmed());
    statusLabel->hide();
    progressBar->hide();
    updateBtn->setText("Update Now");
    updateBtn->show();
    laterBtn->show(); // Not Now -> fall through to the tool stage
    // "Not Now" rejects out of the AppPrompt only; re-route it to the tool stage.
    disconnect(laterBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(laterBtn, &QPushButton::clicked, this, &UpdateDialog::proceedToTools, Qt::UniqueConnection);
}

void UpdateDialog::proceedToTools() {
    // Restore the default "Not Now = reject" wiring for the tool stages.
    disconnect(laterBtn, &QPushButton::clicked, this, &UpdateDialog::proceedToTools);
    connect(laterBtn, &QPushButton::clicked, this, &QDialog::reject, Qt::UniqueConnection);

    if (!m_runToolStage) { accept(); return; } // first run: Setup is the tool stage
    beginCheck();
}

// ---- Stage 2: tools --------------------------------------------------------

void UpdateDialog::beginCheck() {
    m_stage = Stage::Checking;
    m_busy  = true;
    titleLabel->setText("Checking tools");
    bodyLabel->setText("Checking for new versions of yt-dlp, ffmpeg and Deno.");
    statusLabel->setText("Contacting update servers...");
    statusLabel->show();
    progressBar->setRange(0, 0); // indeterminate
    progressBar->show();
    updateBtn->hide();
    laterBtn->hide();

    // Queued invoke so the (blocking) check runs on the updater's thread.
    QMetaObject::invokeMethod(m_updater, [u = m_updater]() { u->checkForUpdates(); },
        Qt::QueuedConnection);
}

void UpdateDialog::onCheckFinished(const QStringList& pending) {
    if (pending.isEmpty()) {
        m_stage = Stage::Done;
        m_busy  = false;
        titleLabel->setText("Up to date");
        bodyLabel->setText("All tools are current.");
        statusLabel->hide();
        progressBar->setRange(0, 100);
        progressBar->setValue(100);
        QTimer::singleShot(900, this, &QDialog::accept);
        return;
    }

    bodyLabel->setText(pending.join("\n"));
    if (m_autoInstall) {
        startUpdate();
        return;
    }

    // Ask-first: unlock just enough to let them choose.
    m_stage = Stage::Prompt;
    m_busy  = false;
    titleLabel->setText("Updates available");
    statusLabel->hide();
    progressBar->hide();
    updateBtn->setText("Update Now");
    updateBtn->show();
    laterBtn->show();
}

void UpdateDialog::startUpdate() {
    m_stage = Stage::Installing;
    m_busy  = true;
    laterBtn->hide();
    updateBtn->hide();
    titleLabel->setText("Installing updates");
    statusLabel->setText("Starting...");
    statusLabel->show();
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->show();

    // Queued invoke so applyUpdates runs on the updater's thread, not the UI's.
    QMetaObject::invokeMethod(m_updater, &SgUpdater::applyUpdates, Qt::QueuedConnection);
}

void UpdateDialog::onProgress(const QString& tool, int percent) {
    statusLabel->setText("Downloading " + tool + "...");
    progressBar->setValue(percent);
}

void UpdateDialog::onFinished(bool allOk) {
    m_stage = Stage::Done;
    m_busy  = false;
    progressBar->setValue(100);
    if (allOk) {
        titleLabel->setText("Updates installed");
        statusLabel->setText("All updates installed.");
        QTimer::singleShot(1200, this, &QDialog::accept);
        return;
    }
    // Leave a failure on screen until dismissed, with a pointer to the log.
    titleLabel->setText("Some updates failed");
    statusLabel->setText("Details are in the Queue log.");
    updateBtn->setText("Close");
    updateBtn->show();
}
