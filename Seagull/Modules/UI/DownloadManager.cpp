#include "DownloadManager.h"
#include "Widgets/DownloadRow.h"
#include "../Backend/SgYtDlp.h"
#include "../Backend/SgDownloadHistory.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QNetworkAccessManager>
#include <QMessageBox>
#include <QFont>
#include <QTimer>
#include <QUrl>
#include <QSettings>
#include "../Backend/SgPaths.h"

DownloadManager::DownloadManager(SgYtDlp* downloadWorker, QWidget* parent)
    : QWidget(parent), m_worker(downloadWorker) {
    m_nam = new QNetworkAccessManager(this);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(50, 30, 50, 30);
    outer->setSpacing(15);

    // Header row: title + clear buttons.
    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Downloads"));
    QFont tf = title->font(); tf.setPointSize(tf.pointSize() + 4); tf.setBold(true);
    title->setFont(tf);
    header->addWidget(title);
    header->addStretch();

    auto* clearDoneBtn = new QPushButton(QStringLiteral("Clear completed"));
    auto* clearAllBtn  = new QPushButton(QStringLiteral("Clear all history"));
    for (QPushButton* b : { clearDoneBtn, clearAllBtn }) {
        b->setObjectName("downloadRowButton");
        b->setCursor(Qt::PointingHandCursor);
        header->addWidget(b);
    }
    outer->addLayout(header);

    // The scrollable list box.
    auto* scroll = new QScrollArea();
    scroll->setObjectName("downloadList");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* host = new QWidget();
    host->setObjectName("downloadListHost");
    m_listLayout = new QVBoxLayout(host);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(10);
    m_listLayout->addStretch(); // rows insert above this so they stack top-down

    m_emptyLabel = new QLabel(QStringLiteral("No downloads yet. Downloads started from search results or the Queue tab will show up here."));
    m_emptyLabel->setObjectName("metaStats"); // themed dim text
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_listLayout->insertWidget(0, m_emptyLabel);

    scroll->setWidget(host);
    outer->addWidget(scroll, 1);

    // Clear actions.
    connect(clearDoneBtn, &QPushButton::clicked, this,
            []() { SgDownloadHistory::instance()->clearFinished(); });
    connect(clearAllBtn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, "Clear download history",
                "Remove every download from this list? This does not delete any files you've "
                "already downloaded.\n\nAny download still running will be stopped.",
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        // Tear down the queue/active first so the killed worker's finished is a no-op.
        m_activeKey.clear();
        m_queue.clear();
        m_downloading = false;
        m_canceling = false;
        if (m_worker) m_worker->cancel();
        SgDownloadHistory::instance()->clearAll();
        emit activity(false, -1.0);
    });

    // Worker signals (this module now owns the download worker's output).
    connect(m_worker, &SgYtDlp::downloadProgress,    this, &DownloadManager::onProgress);
    connect(m_worker, &SgYtDlp::downloadDestination, this, &DownloadManager::onDestination);
    connect(m_worker, &SgYtDlp::finished,            this, &DownloadManager::onFinished);

    // Rebuild the list whenever the store changes structurally.
    connect(SgDownloadHistory::instance(), &SgDownloadHistory::changed,
            this, &DownloadManager::rebuild);

    rebuild();
}

void DownloadManager::enqueue(const QUrl& pageUrl, const QString& title, const QString& thumbUrl) {
    const QString url = pageUrl.toString();
    if (url.isEmpty()) return;

    // The download's shape is whatever the Settings download section says right now —
    // capture it on the record, both for the row's type label and so the same link can
    // be downloaded again as something else (video AND audio = two separate rows).
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    const QString kind = settings.value("Download/Type", "Video").toString();
    const QString fmt  = settings.value("Download/Format", "mp4").toString();

    // Ignore a re-click while the identical download is already queued or running.
    if (!SgDownloadHistory::instance()->pendingDuplicate(url, kind, fmt).isEmpty()) return;

    const QString id = SgDownloadHistory::instance()->add(url, title, thumbUrl, kind, fmt);
    m_queue.append(id);
    pump();
}

void DownloadManager::pump() {
    if (m_downloading || m_queue.isEmpty() || !m_worker) return;
    m_activeKey   = m_queue.first();
    m_downloading = true;
    m_canceling   = false;
    SgDownloadHistory::instance()->setStatus(m_activeKey, SgDownloadHistory::Downloading);
    m_worker->download(SgDownloadHistory::instance()->recordFor(m_activeKey).pageUrl);
    emit activity(true, 0.0);
}

void DownloadManager::onProgress(double percent, const QString& speed, const QString& eta) {
    if (m_activeKey.isEmpty()) return;
    if (DownloadRow* r = m_rows.value(m_activeKey)) r->setLiveProgress(percent, speed, eta);
    emit activity(true, percent);
}

void DownloadManager::onDestination(const QString& path) {
    if (!m_activeKey.isEmpty())
        SgDownloadHistory::instance()->setFilePath(m_activeKey, path);
}

void DownloadManager::onFinished(bool ok) {
    if (m_activeKey.isEmpty()) { m_downloading = false; return; } // killed by an admin action
    const QString key = m_activeKey;
    const int status = m_canceling ? SgDownloadHistory::Canceled
                                    : (ok ? SgDownloadHistory::Completed : SgDownloadHistory::Failed);
    m_queue.removeOne(key);
    m_downloading = false;
    m_activeKey.clear();
    m_canceling = false;
    SgDownloadHistory::instance()->setStatus(key, status); // triggers rebuild
    if (!m_queue.isEmpty())
        // Defer: a user Cancel reaches here synchronously inside SgYtDlp::cancel()'s
        // waitForFinished(), which sets the worker Idle only AFTER we return. Starting the
        // next download now would have that reset clobber it, so let the stack unwind first.
        QTimer::singleShot(0, this, [this]() { pump(); });
    else emit activity(false, -1.0);
}

void DownloadManager::restart(const QString& id) {
    if (id == m_activeKey) return; // already running
    SgDownloadHistory::instance()->requeue(id); // same row, back to Queued
    if (!m_queue.contains(id)) m_queue.append(id);
    pump();
}

void DownloadManager::cancel(const QString& id) {
    if (id == m_activeKey) {
        // Kill the running process; onFinished sees m_canceling and marks it Canceled.
        m_canceling = true;
        if (m_worker) m_worker->cancel();
    } else {
        m_queue.removeOne(id);
        SgDownloadHistory::instance()->setStatus(id, SgDownloadHistory::Canceled);
    }
}

void DownloadManager::removeOne(const QString& id) {
    m_queue.removeOne(id);
    SgDownloadHistory::instance()->remove(id);
}

void DownloadManager::rebuild() {
    // Drop the current rows (keep the stretch + empty label).
    for (DownloadRow* r : m_rows) { m_listLayout->removeWidget(r); r->deleteLater(); }
    m_rows.clear();

    const QList<SgDownloadHistory::Record> recs = SgDownloadHistory::instance()->records();
    m_emptyLabel->setVisible(recs.isEmpty());

    // Insert newest-first, above the trailing stretch (index grows as we add).
    int idx = 0;
    for (const SgDownloadHistory::Record& rec : recs) {
        auto* row = new DownloadRow(rec, m_nam);
        connect(row, &DownloadRow::restartRequested,    this, &DownloadManager::restart);
        connect(row, &DownloadRow::cancelRequested,     this, &DownloadManager::cancel);
        connect(row, &DownloadRow::removeRequested,     this, &DownloadManager::removeOne);
        connect(row, &DownloadRow::openFolderRequested, this,
                [this](const QString& path) { if (!path.isEmpty()) emit openFileRequested(path); });
        m_listLayout->insertWidget(idx++, row);
        m_rows.insert(rec.id, row);
    }
}
