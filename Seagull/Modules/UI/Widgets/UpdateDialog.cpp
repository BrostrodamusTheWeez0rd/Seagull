#include "UpdateDialog.h"
#include "../../Backend/SgUpdater.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QFont>

UpdateDialog::UpdateDialog(SgUpdater* updater, const QStringList& pending, QWidget* parent)
    : QDialog(parent), m_updater(updater) {
    setWindowTitle("Seagull — Tool Updates");
    setModal(true);
    setMinimumWidth(420);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(12);

    auto* title = new QLabel("Updates available", this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() + 3);
    tf.setBold(true);
    title->setFont(tf);
    lay->addWidget(title);

    auto* body = new QLabel(pending.join("\n"), this);
    body->setTextFormat(Qt::PlainText);
    lay->addWidget(body);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("metaStats"); // theme's dimmed text styling
    statusLabel->hide();
    lay->addWidget(statusLabel);

    progressBar = new QProgressBar(this);  // themed by the global sheet
    progressBar->setRange(0, 100);
    progressBar->hide();
    lay->addWidget(progressBar);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    laterBtn = new QPushButton("Not Now", this);
    updateBtn = new QPushButton("Update Now", this);
    updateBtn->setDefault(true);
    btnRow->addWidget(laterBtn);
    btnRow->addWidget(updateBtn);
    lay->addLayout(btnRow);

    connect(laterBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(updateBtn, &QPushButton::clicked, this, &UpdateDialog::startUpdate);

    // Cross-thread: the updater emits from its own thread, so these arrive queued.
    connect(m_updater, &SgUpdater::applyProgress, this, &UpdateDialog::onProgress);
    connect(m_updater, &SgUpdater::applyFinished, this, &UpdateDialog::onFinished);
}

void UpdateDialog::startUpdate() {
    updateBtn->setEnabled(false);
    laterBtn->setEnabled(false);
    statusLabel->setText("Starting...");
    statusLabel->show();
    progressBar->show();

    // Queued invoke so applyUpdates runs on the updater's thread, not the UI's.
    QMetaObject::invokeMethod(m_updater, &SgUpdater::applyUpdates, Qt::QueuedConnection);
}

void UpdateDialog::onProgress(const QString& tool, int percent) {
    statusLabel->setText("Downloading " + tool + "...");
    progressBar->setValue(percent);
}

void UpdateDialog::onFinished(bool allOk) {
    progressBar->setValue(100);
    statusLabel->setText(allOk ? "All updates installed."
                               : "Some updates failed — details in the Queue log.");
    laterBtn->hide();
    updateBtn->setText("Close");
    updateBtn->setEnabled(true);
    disconnect(updateBtn, &QPushButton::clicked, this, &UpdateDialog::startUpdate);
    connect(updateBtn, &QPushButton::clicked, this, &QDialog::accept);
}
