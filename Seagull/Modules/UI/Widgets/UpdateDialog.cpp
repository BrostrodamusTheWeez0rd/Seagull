#include "UpdateDialog.h"
#include "../../Backend/SgUpdater.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QFont>

UpdateDialog::UpdateDialog(SgUpdater* updater, bool autoInstall, QWidget* parent)
    : QDialog(parent), m_updater(updater), m_autoInstall(autoInstall) {
    setWindowTitle("Seagull Tool Updates");
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

    // Cross-thread: the updater emits from its own thread, so these arrive queued.
    connect(m_updater, &SgUpdater::checkFinished, this, &UpdateDialog::onCheckFinished);
    connect(m_updater, &SgUpdater::applyProgress, this, &UpdateDialog::onProgress);
    connect(m_updater, &SgUpdater::applyFinished, this, &UpdateDialog::onFinished);

    // Auto-update goes straight into the check; ask-first offers it instead.
    if (m_autoInstall) beginCheck();
    else               enterAskStage();
}

void UpdateDialog::reject() {
    if (m_busy) return; // locked while checking/installing
    QDialog::reject();
}

void UpdateDialog::onPrimaryClicked() {
    switch (m_stage) {
    case Stage::Ask:    beginCheck();  break;
    case Stage::Prompt: startUpdate(); break;
    case Stage::Done:   accept();      break;
    default: break; // busy stages have no primary button showing
    }
}

void UpdateDialog::enterAskStage() {
    m_stage = Stage::Ask;
    m_busy  = false;
    titleLabel->setText("Check for updates?");
    bodyLabel->setText(
        "Look for new versions of yt-dlp, ffmpeg and Deno now?\n"
        "Seagull stays locked while a check or install runs, so a tool is "
        "never replaced mid-use.");
    statusLabel->hide();
    progressBar->hide();
    updateBtn->setText("Check Now");
    updateBtn->show();
    laterBtn->show();
}

void UpdateDialog::beginCheck() {
    m_stage = Stage::Checking;
    m_busy  = true;
    m_checkStarted = true; // a check is happening (auto, or user accepted the ask)
    titleLabel->setText("Checking for updates");
    bodyLabel->setText("Making sure yt-dlp, ffmpeg and Deno are current.");
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
