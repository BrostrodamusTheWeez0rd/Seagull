#include "Downloads.h"
#include <QFont>
#include <QPixmap>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QUrlQuery>
#include <QDebug>
#include <algorithm>

Downloads::Downloads(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->setSpacing(15);
    layout->setContentsMargins(50, 30, 50, 30);

    downloader = new SgYtDlp(this);
    titleResolver = new SgYtDlp(this);
    cdnPrefetcher = new SgYtDlp(this);
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

    downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
    processQueueBtn->hide(); streamQueueBtn->hide();

    downBtn->setMinimumHeight(30); processQueueBtn->setMinimumHeight(30);
    queueBtn->setMinimumHeight(30); streamBtn->setMinimumHeight(30);
    streamQueueBtn->setMinimumHeight(30);

    btnLayout->addStretch();
    btnLayout->addWidget(downBtn); btnLayout->addWidget(processQueueBtn);
    btnLayout->addWidget(queueBtn); btnLayout->addWidget(streamBtn);
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

    layout->addWidget(banner); layout->addWidget(loadingLabel);
    layout->addWidget(metadataContainer); layout->addWidget(urlInput);
    layout->addLayout(btnLayout); layout->addWidget(queueTable);
    layout->addWidget(progressBar); layout->addWidget(logConsole);

    debounceTimer = new QTimer(this);
    debounceTimer->setSingleShot(true);
    resolverTimer = new QTimer(this);
    resolverTimer->setSingleShot(true);

    connect(downloader, &SgYtDlp::logMessage, this, &Downloads::handleLogMessage);
    connect(downloader, &SgYtDlp::progressUpdated, this, &Downloads::handleProgress);
    connect(downloader, &SgYtDlp::finished, this, &Downloads::handleFinished);
    connect(downloader, &SgYtDlp::metadataReady, this, &Downloads::handleMetadataReady);
    connect(downloader, &SgYtDlp::streamUrlReady, this, &Downloads::handleStreamUrlReady);
    connect(downloader, &SgYtDlp::playlistEntriesReady, this, &Downloads::handlePlaylistEntriesReady);

    connect(titleResolver, &SgYtDlp::metadataReady, this, &Downloads::handleResolverMetadataReady);
    connect(cdnPrefetcher, &SgYtDlp::streamUrlReady, this, &Downloads::handlePrefetchedStreamUrlReady);

    connect(debounceTimer, &QTimer::timeout, this, &Downloads::triggerMetadataFetch);
    connect(resolverTimer, &QTimer::timeout, this, &Downloads::resolveNextTitle);
    connect(urlInput, &QLineEdit::textChanged, this, &Downloads::onUrlTextChanged);
    connect(downBtn, &QPushButton::clicked, this, &Downloads::onDownloadClicked);
    connect(queueBtn, &QPushButton::clicked, this, &Downloads::onAddToQueueClicked);
    connect(processQueueBtn, &QPushButton::clicked, this, &Downloads::onProcessQueueClicked);
    connect(streamBtn, &QPushButton::clicked, this, &Downloads::onStreamClicked);
    connect(streamQueueBtn, &QPushButton::clicked, this, &Downloads::onStreamQueueClicked);
    connect(queueTable, &QTableWidget::customContextMenuRequested, this, &Downloads::showContextMenu);
}

// ---------------------------------------------------------------------------
// Helpers & Expiration Validation
// ---------------------------------------------------------------------------

bool Downloads::isPlaylistUrl(const QString& url) const {
    return !QUrlQuery(QUrl(url).query()).queryItemValue("list").isEmpty();
}

QString Downloads::stripToVideoUrl(const QString& url) const {
    QUrl qurl(url);
    QString videoId = QUrlQuery(qurl.query()).queryItemValue("v");
    if (videoId.isEmpty()) return url;
    QUrlQuery stripped;
    stripped.addQueryItem("v", videoId);
    qurl.setQuery(stripped);
    return qurl.toString();
}

bool Downloads::isStreamUrlValid(const QUrl& cdnUrl) const {
    if (cdnUrl.isEmpty()) return false;
    QUrlQuery query(cdnUrl.query());
    QString expireStr = query.queryItemValue("expire");
    if (expireStr.isEmpty()) return false;

    qint64 expireTime = expireStr.toLongLong();
    qint64 currentTime = QDateTime::currentSecsSinceEpoch();

    return (expireTime - 300) > currentTime;
}

void Downloads::offerPlaylistQueue(const QString& fullUrl) {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Playlist Detected");
    msgBox.setText("This URL contains a playlist.\n\nWould you like to add all playlist items to the queue?");
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::Yes);
    if (msgBox.exec() == QMessageBox::Yes) {
        downloader->fetchPlaylistEntries(fullUrl);
    }
}

void Downloads::updateQueueButtonVisibility() {
    bool hasItems = (queueTable->rowCount() > 0);
    processQueueBtn->setVisible(hasItems);
    streamQueueBtn->setVisible(hasItems);
}

// ---------------------------------------------------------------------------
// CDN Pre-Fetching & Title Resolution
// ---------------------------------------------------------------------------

void Downloads::enqueueTitleResolution(const QList<QString>& urls, int startRow) {
    for (int i = 0; i < urls.size(); ++i) m_titleQueue.append({ startRow + i, urls[i] });
    if (m_currentResolvingRow == -1) resolverTimer->start(0);
}

void Downloads::resolveNextTitle() {
    if (m_titleQueue.isEmpty()) {
        m_currentResolvingRow = -1;
        return;
    }
    auto [row, url] = m_titleQueue.takeFirst();
    if (row >= queueTable->rowCount()) {
        resolverTimer->start(1500);
        return;
    }
    m_currentResolvingRow = row;
    titleResolver->fetchMetadataAndStreamUrl(url);
}

void Downloads::handleResolverMetadataReady(const QString& title, const QString&, const QString&, const QString&, const QString&, const QString&) {
    if (m_currentResolvingRow >= 0 && m_currentResolvingRow < queueTable->rowCount()) {
        QTableWidgetItem* item = queueTable->item(m_currentResolvingRow, 0);
        if (item) item->setText(title.isEmpty() ? item->data(Qt::UserRole).toString() : title);
    }
    m_currentResolvingRow = -1;
    if (!m_titleQueue.isEmpty()) resolverTimer->start(1500);
}

void Downloads::prefetchNextInQueue() {
    if (!m_currentlyPrefetchingUrl.isEmpty()) return;

    for (int i = 0; i < queueTable->rowCount(); ++i) {
        QString url = queueTable->item(i, 0)->data(Qt::UserRole).toString();
        if (url.isEmpty()) url = queueTable->item(i, 0)->text();
        QString cleanUrl = stripToVideoUrl(url);

        // Check if we already have a valid cache for this item (checking the video link validity)
        if (!cdnCache.contains(cleanUrl) || !isStreamUrlValid(cdnCache[cleanUrl].first)) {
            m_currentlyPrefetchingUrl = cleanUrl;
            logConsole->append("Prefetching zero-latency CDN stream link...");
            cdnPrefetcher->fetchMetadataAndStreamUrl(cleanUrl);
            return;
        }
    }
}

void Downloads::handlePrefetchedStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    if (!m_currentlyPrefetchingUrl.isEmpty()) {
        cdnCache[m_currentlyPrefetchingUrl] = qMakePair(videoUrl, audioUrl);
        logConsole->append("CDN Video & Audio Links cached and ready for instant playback.");
        m_currentlyPrefetchingUrl.clear();
    }
    QTimer::singleShot(2000, this, &Downloads::prefetchNextInQueue);
}

// ---------------------------------------------------------------------------
// Primary Interactions
// ---------------------------------------------------------------------------

void Downloads::onUrlTextChanged(const QString& text) {
    if (text == "Bdev") { logConsole->setVisible(!logConsole->isVisible()); urlInput->clear(); return; }
    if (text.startsWith("http")) {
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        metadataContainer->hide(); loadingLabel->show();
        cachedTitle.clear();
        isFetchingMetadata = false; m_pendingPlaylistUrl.clear();
        if (isPlaylistUrl(text)) m_pendingPlaylistUrl = text;
        debounceTimer->start(800);
    }
    else {
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        loadingLabel->hide(); m_pendingPlaylistUrl.clear();
    }
}

void Downloads::triggerMetadataFetch() {
    isFetchingMetadata = true;
    QString fetchUrl = m_pendingPlaylistUrl.isEmpty() ? urlInput->text() : stripToVideoUrl(urlInput->text());
    downloader->fetchMetadataAndStreamUrl(fetchUrl);
}

void Downloads::onDownloadClicked() {
    isProcessingQueue = false; progressBar->show(); logConsole->clear();
    downloader->download(urlInput->text());
}

void Downloads::onAddToQueueClicked() {
    if (urlInput->text().isEmpty()) return;
    int row = queueTable->rowCount();
    queueTable->insertRow(row);

    auto* item = new QTableWidgetItem(cachedTitle.isEmpty() ? urlInput->text() : cachedTitle);
    item->setData(Qt::UserRole, urlInput->text());
    queueTable->setItem(row, 0, item);
    queueTable->setItem(row, 1, new QTableWidgetItem("Queued"));
    queueTable->setItem(row, 2, new QTableWidgetItem("0%"));

    urlInput->clear(); metadataContainer->hide(); cachedTitle.clear(); m_pendingPlaylistUrl.clear();
    updateQueueButtonVisibility();
    prefetchNextInQueue();
}

void Downloads::onProcessQueueClicked() {
    if (queueTable->rowCount() == 0) return;
    isProcessingQueue = true;
    processQueueBtn->setEnabled(false); streamQueueBtn->setEnabled(false);
    QString url = queueTable->item(0, 0)->data(Qt::UserRole).toString();
    downloader->download(url);
    progressBar->show();
}

void Downloads::onStreamClicked() {
    if (urlInput->text().isEmpty()) return;
    QString cleanUrl = stripToVideoUrl(urlInput->text());
    QString title = cachedTitle.isEmpty() ? "Streaming..." : cachedTitle;

    QUrl cdnVideoUrl, cdnAudioUrl;
    if (cdnCache.contains(cleanUrl) && isStreamUrlValid(cdnCache[cleanUrl].first)) {
        cdnVideoUrl = cdnCache[cleanUrl].first;
        cdnAudioUrl = cdnCache[cleanUrl].second;
    }

    emit playMediaRequested(QUrl(cleanUrl), cdnVideoUrl, cdnAudioUrl, title);
}

void Downloads::onStreamQueueClicked() {
    if (queueTable->rowCount() == 0) return;
    QString url = queueTable->item(0, 0)->data(Qt::UserRole).toString();
    if (url.isEmpty()) url = queueTable->item(0, 0)->text();
    QString cleanUrl = stripToVideoUrl(url);

    QUrl cdnVideoUrl, cdnAudioUrl;
    if (cdnCache.contains(cleanUrl) && isStreamUrlValid(cdnCache[cleanUrl].first)) {
        cdnVideoUrl = cdnCache[cleanUrl].first;
        cdnAudioUrl = cdnCache[cleanUrl].second;
    }

    emit playMediaRequested(QUrl(cleanUrl), cdnVideoUrl, cdnAudioUrl, "Streaming Queue...");
    prefetchNextInQueue();
}

// ---------------------------------------------------------------------------
// Queue Context Menu Operations
// ---------------------------------------------------------------------------

void Downloads::showContextMenu(const QPoint& pos) {
    QModelIndex index = queueTable->indexAt(pos);
    if (index.isValid()) queueTable->setCurrentIndex(index);
    else queueTable->clearSelection();

    QMenu menu(this);
    if (queueTable->selectionModel()->hasSelection()) {
        menu.addAction("Play", this, &Downloads::playSelectedItem);
        menu.addAction("Download", this, &Downloads::downloadSelectedItems);
        menu.addAction("Remove", this, &Downloads::removeSelectedItems);
    }
    menu.exec(queueTable->mapToGlobal(pos));
}

void Downloads::playSelectedItem() {
    auto selected = queueTable->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    int row = selected.first().row();
    QTableWidgetItem* item = queueTable->item(row, 0);

    QString url = item->data(Qt::UserRole).toString();
    if (url.isEmpty()) url = item->text();
    QString cleanUrl = stripToVideoUrl(url);
    QString title = item->text();

    QUrl cdnVideoUrl, cdnAudioUrl;
    if (cdnCache.contains(cleanUrl) && isStreamUrlValid(cdnCache[cleanUrl].first)) {
        cdnVideoUrl = cdnCache[cleanUrl].first;
        cdnAudioUrl = cdnCache[cleanUrl].second;
    }

    emit playMediaRequested(QUrl(cleanUrl), cdnVideoUrl, cdnAudioUrl, title);
    prefetchNextInQueue();
}

void Downloads::downloadSelectedItems() {
    auto selected = queueTable->selectionModel()->selectedRows();
    for (const auto& index : selected) {
        QTableWidgetItem* item = queueTable->item(index.row(), 0);
        QString url = item->data(Qt::UserRole).toString();
        if (url.isEmpty()) url = item->text();
        downloader->download(url);
    }
}

void Downloads::removeSelectedItems() {
    auto selected = queueTable->selectionModel()->selectedRows();
    QList<int> rows;
    for (const auto& index : selected) rows.append(index.row());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) queueTable->removeRow(row);
    updateQueueButtonVisibility();
}

// ---------------------------------------------------------------------------
// Callbacks and Signals
// ---------------------------------------------------------------------------

void Downloads::handleMetadataReady(const QString& t, const QString& u, const QString& d,
    const QString& v, const QString& da, const QString& /*th*/) {
    isFetchingMetadata = false; loadingLabel->hide();
    downBtn->setEnabled(true); queueBtn->setEnabled(true); streamBtn->setEnabled(true);
    cachedTitle = t;
    metaTitle->setText(t); metaUploader->setText(u);
    metaStats->setText(QString("Duration: %1 | Views: %2 | Uploaded: %3").arg(d, v, da));
    metadataContainer->show();

    if (!m_pendingPlaylistUrl.isEmpty()) {
        QString playlistUrl = m_pendingPlaylistUrl;
        m_pendingPlaylistUrl.clear();
        offerPlaylistQueue(playlistUrl);
    }
}

void Downloads::handleStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    if (!urlInput->text().isEmpty()) {
        cdnCache[stripToVideoUrl(urlInput->text())] = qMakePair(videoUrl, audioUrl);
    }
}

void Downloads::handlePlaylistEntriesReady(const QList<QString>& urls) {
    int startRow = queueTable->rowCount();
    for (const QString& url : urls) {
        int row = queueTable->rowCount();
        queueTable->insertRow(row);
        auto* item = new QTableWidgetItem(url);
        item->setData(Qt::UserRole, url);
        queueTable->setItem(row, 0, item);
        queueTable->setItem(row, 1, new QTableWidgetItem("Queued"));
        queueTable->setItem(row, 2, new QTableWidgetItem("0%"));
    }
    updateQueueButtonVisibility();
    logConsole->append(QString("Added %1 playlist items. Resolving...").arg(urls.size()));
    enqueueTitleResolution(urls, startRow);
    prefetchNextInQueue();
}

void Downloads::handleFinished(bool /*success*/) {
    if (isProcessingQueue) {
        queueTable->removeRow(0);
        if (queueTable->rowCount() > 0) {
            QString url = queueTable->item(0, 0)->data(Qt::UserRole).toString();
            if (url.isEmpty()) url = queueTable->item(0, 0)->text();
            downloader->download(url);
        }
        else {
            isProcessingQueue = false;
            processQueueBtn->setEnabled(true); streamQueueBtn->setEnabled(true);
            updateQueueButtonVisibility(); progressBar->hide();
        }
    }
    else { progressBar->hide(); }
}

void Downloads::handleLogMessage(const QString& m) {
    logConsole->append(m);
    QTextCursor c = logConsole->textCursor();
    c.movePosition(QTextCursor::End);
    logConsole->setTextCursor(c);
}

void Downloads::handleProgress(double p) {
    progressBar->setValue(static_cast<int>(p));
    if (isProcessingQueue && queueTable->rowCount() > 0) {
        queueTable->setItem(0, 2, new QTableWidgetItem(QString::number(p, 'f', 1) + "%"));
    }
    else {
        int last = queueTable->rowCount() - 1;
        if (last >= 0) queueTable->setItem(last, 2, new QTableWidgetItem(QString::number(p, 'f', 1) + "%"));
    }
}