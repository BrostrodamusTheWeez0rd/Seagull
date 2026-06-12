#include "Search.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "../Backend/SgSearch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QLabel>
#include <QWidget>
#include <QNetworkAccessManager>
#include <QMovie>
#include <QSettings>
#include <QCoreApplication>
#include <QEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QCompleter>
#include <QStringListModel>
#include <QPixmapCache>
#include <QTimer>
#include <QCursor>

namespace { constexpr int kGridSpacing = 12; }

Search::Search(SgSearch* searchWorker, QWidget* parent)
    : QWidget(parent), m_search(searchWorker) {
    m_nam = new QNetworkAccessManager(this);

    // Root layout: 0 margins so the separator and results area span full width.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Chrome: nav + query bar (10 px side + top padding) ---
    auto* chromeWidget = new QWidget();
    auto* chromeLay    = new QVBoxLayout(chromeWidget);
    chromeLay->setContentsMargins(10, 10, 10, 6);
    chromeLay->setSpacing(8);

    // Nav buttons + site bar + Go
    auto* navRow = new QHBoxLayout();
    navRow->setSpacing(6);

    backBtn    = new QPushButton(QStringLiteral("‹"));
    forwardBtn = new QPushButton(QStringLiteral("›"));
    refreshBtn = new QPushButton(QStringLiteral("⟳"));
    for (QPushButton* b : { backBtn, forwardBtn, refreshBtn }) {
        b->setObjectName("searchNavButton");
        b->setFixedWidth(34);
        b->setCursor(Qt::PointingHandCursor);
    }
    backBtn->setEnabled(false);
    forwardBtn->setEnabled(false);
    refreshBtn->setEnabled(false);

    siteBar = new QLineEdit();
    siteBar->setObjectName("searchSiteBar");
    siteBar->setPlaceholderText("Site \xe2\x80\x94 e.g. youtube");
    siteBar->setText("youtube");
    siteBar->setClearButtonEnabled(true);

    goBtn = new QPushButton(QStringLiteral("Go"));
    goBtn->setObjectName("searchGoButton");
    goBtn->setCursor(Qt::PointingHandCursor);

    navRow->addWidget(backBtn);
    navRow->addWidget(forwardBtn);
    navRow->addWidget(refreshBtn);
    navRow->addWidget(siteBar, 1);
    navRow->addWidget(goBtn);
    chromeLay->addLayout(navRow);

    // Query bar with history completer
    queryBar = new QLineEdit();
    queryBar->setObjectName("searchQueryBar");
    queryBar->setPlaceholderText("Search YouTube\xe2\x80\xa6");
    queryBar->setClearButtonEnabled(true);

    m_historyModel     = new QStringListModel(this);
    m_historyCompleter = new QCompleter(m_historyModel, this);
    m_historyCompleter->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_historyCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    queryBar->setCompleter(m_historyCompleter);
    chromeLay->addWidget(queryBar);

    root->addWidget(chromeWidget);

    // --- Full-width separator (browser chrome / page boundary) ---
    auto* sep = new QFrame();
    sep->setObjectName("searchSeparator");
    sep->setFixedHeight(1);
    root->addWidget(sep);

    // --- Results area (full width) ---
    resultsArea = new QScrollArea();
    resultsArea->setWidgetResizable(true);
    resultsArea->setFrameShape(QFrame::NoFrame);
    resultsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Always-on vertical so the scrollbar can't flicker on/off and oscillate the
    // grid width (which would re-flow on a loop).
    resultsArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    resultsArea->viewport()->setAutoFillBackground(true);
    resultsArea->viewport()->setBackgroundRole(QPalette::Window);

    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_targetWidth = qBound(120, cfg.value("Display/CardWidth", 240).toInt(), 480);
    m_cardWidth = m_targetWidth;

    resultsHost = new QWidget();
    resultsFlow = new FlowLayout(resultsHost, 0, kGridSpacing, kGridSpacing);
    resultsArea->setWidget(resultsHost);
    root->addWidget(resultsArea, 1);

    resultsArea->viewport()->installEventFilter(this);

    // --- Floating filter pill: Videos / Shorts ---
    // Child of resultsArea (not Search) so raise() puts it above the viewport,
    // avoiding z-order issues where the viewport repaints over a sibling pill.
    m_filterPill = new QFrame(resultsArea);
    m_filterPill->setObjectName("searchFilterPill");
    auto* pillLay = new QHBoxLayout(m_filterPill);
    pillLay->setContentsMargins(6, 4, 6, 4);
    pillLay->setSpacing(2);

    m_filterVideosBtn = new QPushButton("Videos", m_filterPill);
    m_filterShortsBtn = new QPushButton("Shorts", m_filterPill);
    for (QPushButton* b : { m_filterVideosBtn, m_filterShortsBtn }) {
        b->setObjectName("searchFilterButton");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        pillLay->addWidget(b);
    }

    // Reserve top space in the flow layout so the first card row clears the pill.
    // Done after sizeHint() is meaningful (pill is fully laid out).
    m_filterPill->adjustSize();
    const int pillH = m_filterPill->sizeHint().height();
    resultsFlow->setContentsMargins(10, pillH + 2 * kPillTopMargin, 10, 0);

    pillHoverTimer = new QTimer(this);
    pillHoverTimer->setInterval(150);
    connect(pillHoverTimer, &QTimer::timeout, this, &Search::updateFilterPillVisibility);

    // --- Status pill ---
    statusMovie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), this);
    statusMovie->jumpToFrame(0);
    const QSize frame = statusMovie->currentPixmap().size();
    const int spinH = 22;
    const int spinW = frame.height() > 0 ? frame.width() * spinH / frame.height() : spinH;
    statusMovie->setScaledSize(QSize(spinW, spinH));
    statusSpinner = new QLabel();
    statusSpinner->setMovie(statusMovie);
    statusSpinner->hide();

    statusLabel = new QLabel("Search YouTube to see results.");
    statusLabel->setObjectName("searchStatus");

    statusPill = new QFrame();
    statusPill->setObjectName("searchStatusPill");
    auto* pillInner = new QHBoxLayout(statusPill);
    pillInner->setContentsMargins(16, 7, 16, 7);
    pillInner->setSpacing(8);
    pillInner->addWidget(statusLabel);
    pillInner->addWidget(statusSpinner);

    auto* statusFrame = new QWidget();
    auto* statusLay   = new QHBoxLayout(statusFrame);
    statusLay->setContentsMargins(10, 4, 10, 10);
    statusLay->addStretch(1);
    statusLay->addWidget(statusPill);
    statusLay->addStretch(1);
    root->addWidget(statusFrame);

    // --- Connections ---
    connect(goBtn,    &QPushButton::clicked,     this, &Search::performSearch);
    connect(queryBar, &QLineEdit::returnPressed, this, &Search::performSearch);
    connect(siteBar,  &QLineEdit::returnPressed, this, [this]() { queryBar->setFocus(); });

    connect(backBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex <= 0) return;
        --m_navIndex;
        queryBar->setText(m_navHistory[m_navIndex]);
        startSearch(m_navHistory[m_navIndex]);
        updateNavButtons();
    });

    connect(forwardBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex >= m_navHistory.size() - 1) return;
        ++m_navIndex;
        queryBar->setText(m_navHistory[m_navIndex]);
        startSearch(m_navHistory[m_navIndex]);
        updateNavButtons();
    });

    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        if (m_currentQuery.isEmpty()) return;
        for (const SearchResult& r : m_allResults)
            if (!r.thumbnail.isEmpty()) QPixmapCache::remove(r.thumbnail);
        startSearch(m_currentQuery);
    });

    // Filter pill: clicking the active button unchecks it (= show all).
    connect(m_filterVideosBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) m_filterShortsBtn->setChecked(false);
        setFilterMode(checked ? FilterMode::Videos : FilterMode::All);
    });
    connect(m_filterShortsBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) m_filterVideosBtn->setChecked(false);
        setFilterMode(checked ? FilterMode::Shorts : FilterMode::All);
    });

    connect(resultsArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this](int) { updateFilterPillVisibility(); });
    connect(resultsArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar* sb = resultsArea->verticalScrollBar();
        if (sb->maximum() > 0 && value >= sb->maximum() - 48)
            loadMore();
    });

    if (m_search) {
        connect(m_search, &SgSearch::resultsReady, this, &Search::onResultsReady);
        connect(m_search, &SgSearch::failed,       this, &Search::onSearchFailed);
    }
}

// ---------------------------------------------------------------------------
// Widget lifecycle
// ---------------------------------------------------------------------------

void Search::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    positionFilterPill();
    updateFilterPillVisibility();
    pillHoverTimer->start();
}

void Search::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    pillHoverTimer->stop();
}

void Search::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_filterPill->raise(); // keep above the viewport after any layout pass
}

void Search::positionFilterPill() {
    m_filterPill->adjustSize();
    // Coordinates are relative to resultsArea (the pill's parent).
    m_filterPill->move(10, kPillTopMargin);
    m_filterPill->raise();
}

void Search::updateFilterPillVisibility() {
    const bool atTop = (resultsArea->verticalScrollBar()->value() <= 0);
    // Zone is relative to resultsArea (the pill's parent).
    const QRect zone(0, 0, resultsArea->width(), m_filterPill->height() + 2 * kPillTopMargin);
    const bool hovered = zone.contains(resultsArea->mapFromGlobal(QCursor::pos()));
    m_filterPill->setVisible(atTop || hovered);
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

void Search::pushNavEntry(const QString& query) {
    if (m_navIndex < m_navHistory.size() - 1)
        m_navHistory = m_navHistory.mid(0, m_navIndex + 1);
    m_navHistory.append(query);
    m_navIndex = m_navHistory.size() - 1;
    updateNavButtons();
}

void Search::updateNavButtons() {
    backBtn->setEnabled(m_navIndex > 0);
    forwardBtn->setEnabled(m_navIndex < m_navHistory.size() - 1);
}

void Search::addToHistory(const QString& query) {
    m_searchHistory.removeAll(query);
    m_searchHistory.prepend(query);
    if (m_searchHistory.size() > 50) m_searchHistory.removeLast();
    m_historyModel->setStringList(m_searchHistory);
}

// ---------------------------------------------------------------------------
// Search execution
// ---------------------------------------------------------------------------

void Search::performSearch() {
    const QString query = queryBar->text().trimmed();

    if (!siteIsYoutube()) {
        setStatus("Only YouTube is supported for now — try \"youtube\" in the site bar.", false);
        return;
    }
    if (query.isEmpty()) {
        setStatus("Enter something to search for.", false);
        return;
    }
    if (!m_search) {
        setStatus("Search backend unavailable.", false);
        return;
    }

    pushNavEntry(query);
    addToHistory(query);
    startSearch(query);
}

void Search::startSearch(const QString& query) {
    clearResults();
    m_currentQuery = query;
    m_shownCount   = 0;
    m_loadingMore  = false;
    m_endReached   = false;
    refreshBtn->setEnabled(true);

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_batchSize = qBound(5, settings.value("Search/ResultLimit", 20).toInt(), 100);
    m_lastRequested = m_batchSize;
    setStatus("Fetching results.", true);
    m_search->search(SgSearch::Site::YouTube, query, m_lastRequested,
                     m_filterMode == FilterMode::Shorts);
}

void Search::loadMore() {
    if (m_loadingMore || m_endReached || m_shownCount == 0 || !m_search) return;
    if (m_shownCount >= kMaxResults) { m_endReached = true; return; }

    m_loadingMore = true;
    m_lastRequested = qMin(m_shownCount + m_batchSize, kMaxResults);
    setStatus("Loading more results", true);
    m_search->search(SgSearch::Site::YouTube, m_currentQuery, m_lastRequested,
                     m_filterMode == FilterMode::Shorts);
}

// ---------------------------------------------------------------------------
// Result handling
// ---------------------------------------------------------------------------

void Search::onResultsReady(const QList<SearchResult>& results) {
    m_loadingMore = false;

    if (results.size() < m_lastRequested || results.size() <= m_shownCount)
        m_endReached = true;

    if (m_shownCount == 0 && results.isEmpty()) {
        setStatus("No results.", false);
        return;
    }

    for (int i = m_shownCount; i < results.size(); ++i) {
        m_allResults.append(results[i]);
        if (passesFilter(results[i])) addCard(results[i]);
    }
    m_shownCount = results.size();

    if (m_shownCount >= kMaxResults) m_endReached = true;

    applyCardWidth();
    setStatus("", false);

    // If filtered results don't fill the viewport yet, keep loading automatically.
    if (!m_endReached) {
        QScrollBar* sb = resultsArea->verticalScrollBar();
        if (sb->maximum() == 0) loadMore();
    }
}

void Search::onSearchFailed(const QString& message) {
    m_loadingMore = false;
    if (m_shownCount > 0) {
        m_endReached = true;
        setStatus("", false);
    }
    else {
        clearResults();
        setStatus(message, false);
    }
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

bool Search::passesFilter(const SearchResult& r) const {
    switch (m_filterMode) {
    case FilterMode::Videos:  return !r.isShort;
    case FilterMode::Shorts:  return r.isShort;
    default:                  return true;
    }
}

void Search::setFilterMode(FilterMode mode) {
    if (m_filterMode == mode) return;
    const bool crossesShorts = (m_filterMode == FilterMode::Shorts)
                            || (mode == FilterMode::Shorts);
    m_filterMode = mode;
    m_filterVideosBtn->setChecked(mode == FilterMode::Videos);
    m_filterShortsBtn->setChecked(mode == FilterMode::Shorts);

    // Shorts come from a different source (YouTube's shorts search, not the
    // yt-dlp video search), so crossing into or out of Shorts re-runs the
    // query; Videos <-> All just re-filter the cached results.
    if (crossesShorts && !m_currentQuery.isEmpty()) {
        startSearch(m_currentQuery);
        return;
    }
    rebuildCards();

    if (!m_endReached && !m_allResults.isEmpty()) {
        QScrollBar* sb = resultsArea->verticalScrollBar();
        if (sb->maximum() == 0) loadMore();
    }
}

void Search::rebuildCards() {
    QLayoutItem* item;
    while ((item = resultsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    for (const SearchResult& r : m_allResults)
        if (passesFilter(r)) addCard(r);
    applyCardWidth();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool Search::siteIsYoutube() const {
    const QString s = siteBar->text().trimmed().toLower();
    if (s.isEmpty()) return false;
    return s == "yt" || s.contains("youtube") || s.contains("youtu.be");
}

void Search::clearResults() {
    QLayoutItem* item;
    while ((item = resultsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_allResults.clear();
}

void Search::setStatus(const QString& text, bool busy) {
    statusLabel->setText(text);
    statusPill->setVisible(!text.isEmpty());
    if (busy) { statusSpinner->show(); statusMovie->start(); }
    else      { statusMovie->stop();   statusSpinner->hide(); }
}

void Search::addCard(const SearchResult& result) {
    auto* card = new VideoCard(result, m_nam, m_cardWidth, resultsHost,
        VideoCard::AllButtons, QStringLiteral("▶ Stream"));
    connect(card, &VideoCard::playRequested, this, [this](const QUrl& url, const QString& title) {
        emit playMediaRequested(url, QUrl(), QUrl(), title);
    });
    connect(card, &VideoCard::queueRequested,    this, &Search::enqueueRequested);
    connect(card, &VideoCard::downloadRequested, this, &Search::downloadRequested);
    resultsFlow->addWidget(card);
}

int Search::fillCardWidth() const {
    const int vw = resultsArea->viewport()->width();
    const QMargins fm = resultsFlow->contentsMargins();
    const int availW = vw - fm.left() - fm.right();
    const int target = qMax(120, m_targetWidth);
    if (availW <= 0) return target;
    const int cols = qMax(1, (availW + kGridSpacing) / (target + kGridSpacing));
    return (availW - (cols - 1) * kGridSpacing) / cols;
}

void Search::applyCardWidth() {
    const int w = fillCardWidth();
    if (w == m_cardWidth && !resultsFlow->isEmpty()) return;
    m_cardWidth = w;
    for (int i = 0; i < resultsFlow->count(); ++i) {
        if (auto* card = qobject_cast<VideoCard*>(resultsFlow->itemAt(i)->widget()))
            card->setCardWidth(w);
    }
}

void Search::setCardWidth(int targetWidth) {
    m_targetWidth = qBound(120, targetWidth, 480);
    applyCardWidth();
}

bool Search::eventFilter(QObject* obj, QEvent* event) {
    if (obj == resultsArea->viewport() && event->type() == QEvent::Resize)
        applyCardWidth();
    return QWidget::eventFilter(obj, event);
}
