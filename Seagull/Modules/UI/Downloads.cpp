#include "Downloads.h"
#include "../Backend/SgYtDlp.h"
#include <QFont>
#include <QPixmap>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QGraphicsOpacityEffect>
#include <QDebug>
#include <algorithm>

// The workers are owned by the orchestrator and handed in here — we just hold
// the pointers, we don't create or destroy them.
Downloads::Downloads(SgYtDlp* downloaderWorker, SgYtDlp* resolverWorker, SgYtDlp* prefetcherWorker, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->setSpacing(15);
    layout->setContentsMargins(50, 30, 50, 30);

    downloader = downloaderWorker;
    titleResolver = resolverWorker;
    cdnPrefetcher = prefetcherWorker;

    isProcessingQueue = false;
    isStreamingQueue = false;

    banner = new QLabel();
    banner->setAlignment(Qt::AlignCenter);
    banner->setMinimumHeight(120);
    QPixmap bannerImg(":/Assets/Banner.png");
    banner->setPixmap(bannerImg.scaled(800, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // Hero thumbnail: takes the banner's spot once metadata loads. The banner is
    // shrunk into a watermark pinned to the thumbnail's bottom-left corner.
    heroThumb = new QLabel();
    heroThumb->setFixedSize(480, 270);
    heroThumb->setAlignment(Qt::AlignCenter);
    heroThumb->hide();

    bannerWatermark = new QLabel(heroThumb);
    bannerWatermark->setPixmap(QPixmap(":/Assets/Banner.png").scaledToHeight(26, Qt::SmoothTransformation));
    bannerWatermark->adjustSize();
    bannerWatermark->move(8, heroThumb->height() - bannerWatermark->height() - 8);
    auto* wmOpacity = new QGraphicsOpacityEffect(bannerWatermark);
    wmOpacity->setOpacity(0.85);
    bannerWatermark->setGraphicsEffect(wmOpacity);

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

    m_thumbNam = new QNetworkAccessManager(this);

    urlInput = new QLineEdit();
    urlInput->setPlaceholderText("Enter URL here...");
    urlInput->setMinimumHeight(45);

    auto* btnLayout = new QHBoxLayout();
    downBtn = new QPushButton("Download now");
    processQueueBtn = new QPushButton("Download Queue");
    queueBtn = new QPushButton("Add to queue");
    streamBtn = new QPushButton("Stream");
    streamQueueBtn = new QPushButton("Stream Queue");
    clearQueueBtn = new QPushButton("Clear Queue");

    downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
    processQueueBtn->hide(); streamQueueBtn->hide(); clearQueueBtn->hide();

    downBtn->setMinimumHeight(30); processQueueBtn->setMinimumHeight(30);
    queueBtn->setMinimumHeight(30); streamBtn->setMinimumHeight(30);
    streamQueueBtn->setMinimumHeight(30); clearQueueBtn->setMinimumHeight(30);

    btnLayout->addStretch();
    btnLayout->addWidget(downBtn); btnLayout->addWidget(processQueueBtn);
    btnLayout->addWidget(queueBtn); btnLayout->addWidget(streamBtn);
    btnLayout->addWidget(streamQueueBtn); btnLayout->addWidget(clearQueueBtn);
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
    layout->addWidget(heroThumb, 0, Qt::AlignHCenter);
    layout->addWidget(loadingLabel);
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

    connect(titleResolver, &SgYtDlp::logMessage, this, &Downloads::handleLogMessage);
    connect(titleResolver, &SgYtDlp::metadataReady, this, &Downloads::handleResolverMetadataReady);
    connect(titleResolver, &SgYtDlp::streamUrlReady, this, [this](const QUrl& videoUrl, const QUrl& audioUrl) {
        if (m_currentResolvingRow >= 0 && m_currentResolvingRow < queueTable->rowCount()) {
            QTableWidgetItem* item = queueTable->item(m_currentResolvingRow, 0);
            if (item) {
                QString url = item->data(Qt::UserRole).toString();
                if (url.isEmpty()) url = item->text();
                QString cleanUrl = stripToVideoUrl(url);
                if (!cdnCache.contains(cleanUrl) || !isStreamUrlValid(cdnCache[cleanUrl].first)) {
                    cdnCache[cleanUrl] = qMakePair(videoUrl, audioUrl);
                }
            }
        }
        });

    connect(cdnPrefetcher, &SgYtDlp::streamUrlReady, this, &Downloads::handlePrefetchedStreamUrlReady);
    connect(debounceTimer, &QTimer::timeout, this, &Downloads::triggerMetadataFetch);
    connect(resolverTimer, &QTimer::timeout, this, &Downloads::resolveNextTitle);
    connect(urlInput, &QLineEdit::textChanged, this, &Downloads::onUrlTextChanged);
    connect(downBtn, &QPushButton::clicked, this, &Downloads::onDownloadClicked);
    connect(queueBtn, &QPushButton::clicked, this, &Downloads::onAddToQueueClicked);
    connect(processQueueBtn, &QPushButton::clicked, this, &Downloads::onProcessQueueClicked);
    connect(streamBtn, &QPushButton::clicked, this, &Downloads::onStreamClicked);
    connect(streamQueueBtn, &QPushButton::clicked, this, &Downloads::onStreamQueueClicked);
    connect(clearQueueBtn, &QPushButton::clicked, this, &Downloads::onClearQueueClicked);
    connect(queueTable, &QTableWidget::customContextMenuRequested, this, &Downloads::showContextMenu);
}

bool Downloads::isPlaylistUrl(const QString& url) const {
    QUrl qurl(url);
    QUrlQuery query(qurl.query());
    QString path = qurl.path();

    if (query.hasQueryItem("list") || path.startsWith("/playlist")) {
        return true;
    }

    if (path.startsWith("/@") || path.startsWith("/user/") ||
        path.startsWith("/c/") || path.startsWith("/channel/")) {
        return true;
    }

    return false;
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

    QUrl qurl(fullUrl);
    QString videoId = QUrlQuery(qurl.query()).queryItemValue("v");

    // A URL with a "v=" param is a single video that happens to live in a
    // playlist, so let the user pick: the whole list, or just this one video.
    if (!videoId.isEmpty()) {
        msgBox.setText("This link is part of a playlist.\n\nWould you like to add the entire playlist to the queue?\n(Select 'No' to load just the single video)");
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Yes);

        int choice = msgBox.exec();
        if (choice == QMessageBox::Yes) {
            downloader->fetchPlaylistEntries(fullUrl);
        }
        else if (choice == QMessageBox::No) {
            loadingLabel->setText("Analyzing link...");
            loadingLabel->show();
            // Just the one video — drop the list param so yt-dlp doesn't grab the rest.
            downloader->fetchMetadataAndStreamUrl(stripToVideoUrl(fullUrl));
        }
        else {
            loadingLabel->hide();
            m_pendingPlaylistUrl.clear();
        }
    }
    else {
        // No "v=" — this is a pure playlist or channel link.
        msgBox.setText("This URL contains a playlist.\n\nWould you like to add all playlist items to the queue?");
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::Yes);

        if (msgBox.exec() == QMessageBox::Yes) {
            downloader->fetchPlaylistEntries(fullUrl);
        }
        else {
            loadingLabel->hide();
            m_pendingPlaylistUrl.clear();
        }
    }
}

void Downloads::onClearQueueClicked() {
    // Clear state flags FIRST so that killing an in-flight download doesn't
    // trigger queue advancement in handleFinished (which checks isProcessingQueue).
    isStreamingQueue = false;
    isProcessingQueue = false;
    m_waitingForCdn = false;
    m_queuePlayIndex = -1;

    // Stop any download/fetch currently running on the worker.
    downloader->cancel();

    queueTable->setRowCount(0);
    cdnCache.clear();
    m_streamQueue.clear();

    processQueueBtn->setEnabled(true);
    streamQueueBtn->setEnabled(true);
    progressBar->hide();
    updateQueueButtonVisibility();
}

void Downloads::setStreamingQueueMode(bool active) {
    isStreamingQueue = active;
}

void Downloads::playNextQueuedItem() {
    if (!isStreamingQueue) return;
    m_queuePlayIndex++;
    playQueueIndex(m_queuePlayIndex);
}

void Downloads::playPrevQueuedItem() {
    if (!isStreamingQueue) return;
    m_queuePlayIndex = qMax(0, m_queuePlayIndex - 1);
    playQueueIndex(m_queuePlayIndex);
}

void Downloads::playQueueIndex(int index) {
    if (index < 0 || index >= m_streamQueue.size()) {
        isStreamingQueue = false;
        m_queuePlayIndex = -1;
        m_waitingForCdn = false;
        logConsole->append("Stream queue finished.");
        return;
    }

    queueTable->selectRow(index);
    QueueEntry& entry = m_streamQueue[index];

    if (entry.cdnVideoUrl.isEmpty() || !isStreamUrlValid(entry.cdnVideoUrl)) {
        if (cdnCache.contains(entry.rawUrl) && isStreamUrlValid(cdnCache[entry.rawUrl].first)) {
            entry.cdnVideoUrl = cdnCache[entry.rawUrl].first;
            entry.cdnAudioUrl = cdnCache[entry.rawUrl].second;
        }
    }

    if (!entry.cdnVideoUrl.isEmpty() && isStreamUrlValid(entry.cdnVideoUrl)) {
        m_waitingForCdn = false;
        emit playMediaRequested(QUrl(entry.rawUrl), entry.cdnVideoUrl, entry.cdnAudioUrl, entry.title);
        prefetchNextInQueue();
    }
    else {
        m_waitingForCdn = true;
        if (m_currentlyPrefetchingUrl != entry.rawUrl) {
            m_currentlyPrefetchingUrl = entry.rawUrl;
            logConsole->append(QString("Fetching CDN for queue item %1: %2").arg(index).arg(entry.title));
            cdnPrefetcher->fetchMetadataAndStreamUrl(entry.rawUrl);
        }
    }
}

void Downloads::updateQueueButtonVisibility() {
    bool hasItems = (queueTable->rowCount() > 0);
    processQueueBtn->setVisible(hasItems);
    streamQueueBtn->setVisible(hasItems);
    clearQueueBtn->setVisible(hasItems);
}

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

void Downloads::handleResolverMetadataReady(const QString& title, const QString&, const QString&,
    const QString&, const QString&, const QString&) {
    if (m_currentResolvingRow >= 0 && m_currentResolvingRow < queueTable->rowCount()) {
        QTableWidgetItem* item = queueTable->item(m_currentResolvingRow, 0);
        if (item) {
            QString resolved = title.isEmpty() ? item->data(Qt::UserRole).toString() : title;
            item->setText(resolved);

            if (m_currentResolvingRow < m_streamQueue.size())
                m_streamQueue[m_currentResolvingRow].title = resolved;
        }
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
        if (!cdnCache.contains(cleanUrl) || !isStreamUrlValid(cdnCache[cleanUrl].first)) {
            m_currentlyPrefetchingUrl = cleanUrl;
            logConsole->append(QString("Prefetching CDN link for item %1...").arg(i));
            cdnPrefetcher->fetchMetadataAndStreamUrl(cleanUrl);
            return;
        }
    }
}

void Downloads::handlePrefetchedStreamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl) {
    if (!m_currentlyPrefetchingUrl.isEmpty()) {
        cdnCache[m_currentlyPrefetchingUrl] = qMakePair(videoUrl, audioUrl);
        logConsole->append(QString("CDN link cached: %1").arg(m_currentlyPrefetchingUrl));

        for (auto& entry : m_streamQueue) {
            if (entry.rawUrl == m_currentlyPrefetchingUrl) {
                entry.cdnVideoUrl = videoUrl;
                entry.cdnAudioUrl = audioUrl;
                break;
            }
        }

        QString justFetched = m_currentlyPrefetchingUrl;
        m_currentlyPrefetchingUrl.clear();

        if (m_waitingForCdn && isStreamingQueue
            && m_queuePlayIndex >= 0
            && m_queuePlayIndex < m_streamQueue.size()
            && m_streamQueue[m_queuePlayIndex].rawUrl == justFetched) {
            playQueueIndex(m_queuePlayIndex);
            return;
        }
    }

    QTimer::singleShot(2000, this, &Downloads::prefetchNextInQueue);
}

void Downloads::onUrlTextChanged(const QString& text) {
    if (text == "Bdev") { logConsole->setVisible(!logConsole->isVisible()); urlInput->clear(); return; }
    // Any edit invalidates a still-loading thumbnail from the previous link and
    // restores the banner until a new thumbnail is fetched.
    m_currentThumbUrl.clear();
    resetHeroToBanner();
    if (text.startsWith("http")) {
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        metadataContainer->hide();
        loadingLabel->setStyleSheet("color: #aaaaaa; font-style: italic;"); // clear any error styling
        loadingLabel->setText("Analyzing link...");
        loadingLabel->show();
        cachedTitle.clear();
        isFetchingMetadata = false;
        m_pendingPlaylistUrl.clear();
        if (isPlaylistUrl(text)) m_pendingPlaylistUrl = text;
        debounceTimer->start(500);
    }
    else {
        // Empty / non-URL input: clear the preview entirely (label + thumbnail).
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        loadingLabel->hide();
        metadataContainer->hide();
        cachedTitle.clear();
        isFetchingMetadata = false;
        m_pendingPlaylistUrl.clear();
    }
}

void Downloads::triggerMetadataFetch() {
    isFetchingMetadata = true;
    QString fetchUrl = urlInput->text();

    if (isPlaylistUrl(fetchUrl)) {
        offerPlaylistQueue(fetchUrl);
    }
    else {
        downloader->fetchMetadataAndStreamUrl(fetchUrl);
    }

    // Clear this so a stale value can't make handleMetadataReady fire again.
    m_pendingPlaylistUrl.clear();
}

void Downloads::onDownloadClicked() {
    isProcessingQueue = false; progressBar->show(); logConsole->clear();
    // Single-download button: always download just the one video, never the
    // whole playlist, even when the URL carries a &list= parameter.
    downloader->download(stripToVideoUrl(urlInput->text()));
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
    if (url.isEmpty()) url = queueTable->item(0, 0)->text();
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

    m_streamQueue.clear();
    for (int i = 0; i < queueTable->rowCount(); ++i) {
        QTableWidgetItem* item = queueTable->item(i, 0);
        QString url = item->data(Qt::UserRole).toString();
        if (url.isEmpty()) url = item->text();
        QString cleanUrl = stripToVideoUrl(url);

        QueueEntry entry;
        entry.rawUrl = cleanUrl;
        entry.title = item->text();
        if (cdnCache.contains(cleanUrl) && isStreamUrlValid(cdnCache[cleanUrl].first)) {
            entry.cdnVideoUrl = cdnCache[cleanUrl].first;
            entry.cdnAudioUrl = cdnCache[cleanUrl].second;
        }
        m_streamQueue.append(entry);
    }

    m_queuePlayIndex = 0;
    m_waitingForCdn = false;
    isStreamingQueue = true;
    playQueueIndex(0);
}

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
    if (row < 0 || row >= queueTable->rowCount()) return;

    if (isStreamingQueue && row < m_streamQueue.size())
        m_queuePlayIndex = row;

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
    for (int row : rows) {
        if (isStreamingQueue && row < m_queuePlayIndex)
            m_queuePlayIndex--;
        queueTable->removeRow(row);
    }
    updateQueueButtonVisibility();
}

void Downloads::handleMetadataReady(const QString& t, const QString& u, const QString& d,
    const QString& v, const QString& da, const QString& thumbUrl) {
    isFetchingMetadata = false; loadingLabel->hide();
    downBtn->setEnabled(true); queueBtn->setEnabled(true); streamBtn->setEnabled(true);
    cachedTitle = t;
    metaTitle->setText(t); metaUploader->setText(u);
    metaStats->setText(QString("Duration: %1 | Views: %2 | Uploaded: %3").arg(d, v, da));

    // Keep the banner until the thumbnail arrives, then swap it for the hero
    // thumbnail and tuck the banner into the corner as a watermark.
    m_currentThumbUrl = thumbUrl;
    if (!thumbUrl.isEmpty()) {
        QNetworkRequest req((QUrl(thumbUrl)));
        req.setRawHeader("User-Agent", "Seagull-Player");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = m_thumbNam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, thumbUrl]() {
            reply->deleteLater();
            if (thumbUrl != m_currentThumbUrl) return;          // a newer URL superseded this one
            if (reply->error() != QNetworkReply::NoError) return;
            QPixmap pm;
            if (!pm.loadFromData(reply->readAll())) return;     // e.g. webp without the plugin
            heroThumb->setPixmap(pm.scaled(heroThumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            banner->hide();
            heroThumb->show();
            bannerWatermark->show();
            });
    }

    metadataContainer->show();
}

void Downloads::resetHeroToBanner() {
    if (heroThumb) heroThumb->hide();
    if (bannerWatermark) bannerWatermark->hide();
    if (banner) banner->show();
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

    urlInput->clear();
    m_pendingPlaylistUrl.clear();
    isFetchingMetadata = false;
    loadingLabel->hide();

    updateQueueButtonVisibility();
    logConsole->append(QString("Added %1 playlist items.").arg(urls.size()));
    enqueueTitleResolution(urls, startRow);
    prefetchNextInQueue();
}

void Downloads::handleFinished(bool success) {
    // A failure while still fetching the preview metadata means the link couldn't
    // be resolved — surface that instead of leaving "Analyzing link..." forever.
    if (!success && isFetchingMetadata) {
        isFetchingMetadata = false;
        metadataContainer->hide();
        resetHeroToBanner();
        downBtn->setEnabled(false); queueBtn->setEnabled(false); streamBtn->setEnabled(false);
        loadingLabel->setStyleSheet("color: #ff6b6b; font-style: italic;");
        loadingLabel->setText("Couldn't fetch link info — check the URL or your connection.");
        loadingLabel->show();
        progressBar->hide();
        return;
    }

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