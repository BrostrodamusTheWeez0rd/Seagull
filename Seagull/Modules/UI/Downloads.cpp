#include "Downloads.h"
#include <QFont>
#include <QPixmap>
#include <QHeaderView>
#include <QMenu>
#include <QDebug>
#include <algorithm>

Downloads::Downloads(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->setSpacing(15);
    layout->setContentsMargins(50, 30, 50, 30);

    downloader = new SgYtDlp(this);
    isProcessingQueue = false;

    banner = new QLabel();
    banner->setAlignment(Qt::AlignCenter);
    banner->setMinimumHeight(120);
    QPixmap bannerImg(":/Assets/Banner.png");
    banner->setPixmap(bannerImg.scaled(800, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    loadingLabel = new QLabel("Fetching metadata...");
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    loadingLabel->hide();

    metadataContainer = new QWidget();
    auto* metaLayout = new QVBoxLayout(metadataContainer);
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaTitle = new QLabel("Video Title");
    metaTitle->setAlignment(Qt::AlignCenter);
    metaTitle->setStyleSheet("font-size: 16px; font-weight: bold; color: white;");
    metaUploader = new QLabel("Uploader Name");
    metaUploader->setAlignment(Qt::AlignCenter);
    metaUploader->setStyleSheet("font-size: 14px; color: #aaaaaa;");
    metaStats = new QLabel("Duration: -- | Views: -- | Date: --");
    metaStats->setAlignment(Qt::AlignCenter);
    metaStats->setStyleSheet("font-size: 12px; color: #888888;");
    metaLayout->addWidget(metaTitle);
    metaLayout->addWidget(metaUploader);
    metaLayout->addWidget(metaStats);
    metadataContainer->hide();

    urlInput = new QLineEdit();
    urlInput->setPlaceholderText("Enter URL here...");
    urlInput->setMinimumHeight(45);

    auto* btnLayout = new QHBoxLayout();
    downBtn = new QPushButton("Download now");
    processQueueBtn = new QPushButton("Download Queue");
    queueBtn = new QPushButton("Add to queue");
    streamBtn = new QPushButton("Stream");
    streamQueueBtn = new QPushButton("Stream Queue");

    downBtn->setEnabled(false);
    queueBtn->setEnabled(false);
    streamBtn->setEnabled(false);
    processQueueBtn->hide();
    streamQueueBtn->hide();

    downBtn->setMinimumHeight(30);
    processQueueBtn->setMinimumHeight(30);
    queueBtn->setMinimumHeight(30);
    streamBtn->setMinimumHeight(30);
    streamQueueBtn->setMinimumHeight(30);

    btnLayout->addStretch();
    btnLayout->addWidget(downBtn);
    btnLayout->addWidget(processQueueBtn);
    btnLayout->addWidget(queueBtn);
    btnLayout->addWidget(streamBtn);
    btnLayout->addWidget(streamQueueBtn);
    btnLayout->addStretch();

    queueTable = new QTableWidget(0, 3);
    queueTable->setHorizontalHeaderLabels({ "Title", "Status", "Progress" });
    queueTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    queueTable->setMinimumHeight(200);
    queueTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    queueTable->setContextMenuPolicy(Qt::CustomContextMenu);

    progressBar = new QProgressBar();
    progressBar->setRange(0, 100);
    progressBar->hide();

    logConsole = new QTextEdit();
    logConsole->setReadOnly(true);
    logConsole->setStyleSheet("background-color: #1e1e1e; color: #a9a9a9; font-family: monospace;");
    logConsole->setMinimumHeight(150);
    logConsole->hide();

    layout->addWidget(banner);
    layout->addWidget(loadingLabel);
    layout->addWidget(metadataContainer);
    layout->addWidget(urlInput);
    layout->addLayout(btnLayout);
    layout->addWidget(queueTable);
    layout->addWidget(progressBar);
    layout->addWidget(logConsole);

    debounceTimer = new QTimer(this);
    debounceTimer->setSingleShot(true);

    connect(debounceTimer, &QTimer::timeout, this, &Downloads::triggerMetadataFetch);
    connect(urlInput, &QLineEdit::textChanged, this, &Downloads::onUrlTextChanged);
    connect(downBtn, &QPushButton::clicked, this, &Downloads::onDownloadClicked);
    connect(queueBtn, &QPushButton::clicked, this, &Downloads::onAddToQueueClicked);
    connect(processQueueBtn, &QPushButton::clicked, this, &Downloads::onProcessQueueClicked);
    connect(streamBtn, &QPushButton::clicked, this, &Downloads::onStreamClicked);
    connect(streamQueueBtn, &QPushButton::clicked, this, &Downloads::onStreamQueueClicked);
    connect(queueTable, &QTableWidget::customContextMenuRequested, this, &Downloads::showContextMenu);
    connect(downloader, &SgYtDlp::logMessage, this, &Downloads::handleLogMessage);
    connect(downloader, &SgYtDlp::progressUpdated, this, &Downloads::handleProgress);
    connect(downloader, &SgYtDlp::finished, this, &Downloads::handleFinished);
    connect(downloader, &SgYtDlp::metadataReady, this, &Downloads::handleMetadataReady);
    connect(downloader, &SgYtDlp::streamUrlReady, this, &Downloads::handleStreamUrlReady);
}

void Downloads::onStreamQueueClicked() { qDebug() << "Stream queue functionality pending."; }

void Downloads::onUrlTextChanged(const QString& text) {
    if (text == "Bdev") { logConsole->setVisible(!logConsole->isVisible()); urlInput->clear(); return; }
    if (text.startsWith("http")) {
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        metadataContainer->hide();
        loadingLabel->show();
        cachedStreamUrl.clear(); cachedTitle.clear();
        isFetchingMetadata = false;
        debounceTimer->start(800);
    }
    else {
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        loadingLabel->hide();
    }
}

void Downloads::showContextMenu(const QPoint& pos) {
    QModelIndex index = queueTable->indexAt(pos);
    if (index.isValid()) queueTable->setCurrentIndex(index);
    else queueTable->clearSelection();

    QMenu menu(this);
    menu.addAction("Download", this, &Downloads::downloadSelectedItems);
    menu.addAction("Remove", this, &Downloads::removeSelectedItems);
    menu.exec(queueTable->mapToGlobal(pos));
}

void Downloads::downloadSelectedItems() {
    auto selected = queueTable->selectionModel()->selectedRows();
    for (const auto& index : selected) downloader->download(queueTable->item(index.row(), 0)->text());
}

void Downloads::removeSelectedItems() {
    auto selected = queueTable->selectionModel()->selectedRows();
    QList<int> rows;
    for (const auto& index : selected) rows.append(index.row());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) queueTable->removeRow(row);
    if (queueTable->rowCount() == 0) processQueueBtn->hide();
}

void Downloads::onAddToQueueClicked() {
    if (urlInput->text().isEmpty()) return;
    int row = queueTable->rowCount();
    queueTable->insertRow(row);
    queueTable->setItem(row, 0, new QTableWidgetItem(cachedTitle.isEmpty() ? urlInput->text() : cachedTitle));
    queueTable->setItem(row, 1, new QTableWidgetItem("Queued"));
    queueTable->setItem(row, 2, new QTableWidgetItem("0%"));
    urlInput->clear(); metadataContainer->hide(); cachedTitle.clear(); processQueueBtn->show();
}

void Downloads::onProcessQueueClicked() {
    if (queueTable->rowCount() == 0) return;
    isProcessingQueue = true; processQueueBtn->setEnabled(false);
    downloader->download(queueTable->item(0, 0)->text()); progressBar->show();
}

void Downloads::onDownloadClicked() {
    isProcessingQueue = false; progressBar->show(); logConsole->clear();
    downloader->download(urlInput->text());
}

void Downloads::handleFinished(bool success) {
    if (isProcessingQueue) {
        queueTable->removeRow(0);
        if (queueTable->rowCount() > 0) downloader->download(queueTable->item(0, 0)->text());
        else { isProcessingQueue = false; processQueueBtn->hide(); processQueueBtn->setEnabled(true); progressBar->hide(); }
    }
    else { progressBar->hide(); }
}

void Downloads::triggerMetadataFetch() {
    wantsStreamPlayback = false; isFetchingMetadata = true;
    downloader->fetchMetadataAndStreamUrl(urlInput->text());
}

void Downloads::onStreamClicked() {
    if (!cachedStreamUrl.isEmpty()) { emit playMediaRequested(cachedStreamUrl, cachedTitle); return; }
    wantsStreamPlayback = true;
    if (!isFetchingMetadata) {
        debounceTimer->stop(); isFetchingMetadata = true;
        downloader->fetchMetadataAndStreamUrl(urlInput->text());
    }
}

void Downloads::handleMetadataReady(const QString& t, const QString& u, const QString& d, const QString& v, const QString& da, const QString& th) {
    isFetchingMetadata = false; // Kept this to prevent the button from getting stuck!
    loadingLabel->hide();
    downBtn->setEnabled(true); queueBtn->setEnabled(true); streamBtn->setEnabled(true);
    cachedTitle = t;
    metaTitle->setText(t); metaUploader->setText(u);
    metaStats->setText(QString("Duration: %1 | Views: %2 | Uploaded: %3").arg(d, v, da));
    metadataContainer->show();
}

void Downloads::handleStreamUrlReady(const QUrl& u) {
    cachedStreamUrl = u;
    if (wantsStreamPlayback) {
        emit playMediaRequested(u, cachedTitle);
        wantsStreamPlayback = false;
    }
}

void Downloads::handleLogMessage(const QString& m) { logConsole->append(m); QTextCursor c = logConsole->textCursor(); c.movePosition(QTextCursor::End); logConsole->setTextCursor(c); }

void Downloads::handleProgress(double p) {
    progressBar->setValue(static_cast<int>(p));
    if (isProcessingQueue && queueTable->rowCount() > 0) queueTable->setItem(0, 2, new QTableWidgetItem(QString::number(p, 'f', 1) + "%"));
    else { int l = queueTable->rowCount() - 1; if (l >= 0) queueTable->setItem(l, 2, new QTableWidgetItem(QString::number(p, 'f', 1) + "%")); }
}