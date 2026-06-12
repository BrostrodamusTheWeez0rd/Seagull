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

    titleLabel = new QLabel("Checking for updates", this);
    QFont tf = titleLabel->font();
    tf.setPointSizeF(tf.pointSizeF() + 3);
    tf.setBold(true);
    titleLabel->setFont(tf);
    lay->addWidget(titleLabel);

    bodyLabel = new QLabel(
        "Making sure yt-dlp, ffmpeg and Deno are current.\n"
        "Seagull is locked while this runs so a tool is never replaced mid-use.", this);
    bodyLabel->setTextFormat(Qt::PlainText);
    bodyLabel->setWordWrap(true);
    lay->addWidget(bodyLabel);

    statusLabel = new QLabel("Contacting update servers...", this);
    statusLabel->setObjectName("metaStats"); // theme's dimmed text styling
    lay->addWidget(statusLabel);

    progressBar = new QProgressBar(this);  // themed by the global sheet
    progressBar->setRange(0, 0);           // indeterminate while checking
    lay->addWidget(progressBar);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    laterBtn = new QPushButton("Not Now", this);
    updateBtn = new QPushButton("Update Now", this);
    updateBtn->setDefault(true);
    laterBtn->hide();   // buttons only appear in the ask-first prompt state
    updateBtn->hide();
    btnRow->addWidget(laterBtn);
    btnRow->addWidget(updateBtn);
    lay->addLayout(btnRow);

    connect(laterBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(updateBtn, &QPushButton::clicked, this, &UpdateDialog::startUpdate);

    // Cross-thread: the updater emits from its own thread, so these arrive queued.
    connect(m_updater, &SgUpdater::checkFinished, this, &UpdateDialog::onCheckFinished);
    connect(m_updater, &SgUpdater::applyProgress, this, &UpdateDialog::onProgress);
    connect(m_updater, &SgUpdater::applyFinished, this, &UpdateDialog::onFinished);
}

void UpdateDialog::reject() {
    if (m_busy) return; // locked while checking/installing
    QDialog::reject();
}

void UpdateDialog::onCheckFinished(const QStringList& pending) {
    if (pending.isEmpty()) {
        m_busy = false;
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

    // Ask-first (AutoUpdate off): unlock just enough to let them choose.
    m_busy = false;
    titleLabel->setText("Updates available");
    statusLabel->hide();
    progressBar->hide();
    laterBtn->show();
    updateBtn->show();
}

void UpdateDialog::startUpdate() {
    m_busy = true;
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
    m_busy = false;
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
    disconnect(updateBtn, &QPushButton::clicked, this, &UpdateDialog::startUpdate);
    connect(updateBtn, &QPushButton::clicked, this, &QDialog::accept);
    updateBtn->show();
}
