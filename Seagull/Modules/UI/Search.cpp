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

namespace { constexpr int kGridSpacing = 12; }

Search::Search(SgSearch* searchWorker, QWidget* parent)
    : QWidget(parent), m_search(searchWorker) {
    m_nam = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // --- Top row: nav buttons + site bar + Go ---
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

    siteBar = new QLineEdit();
    siteBar->setObjectName("searchSiteBar");
    siteBar->setPlaceholderText("Site — e.g. youtube");
    siteBar->setText("youtube"); // only site supported for now
    siteBar->setClearButtonEnabled(true);

    goBtn = new QPushButton(QStringLiteral("Go"));
    goBtn->setObjectName("searchGoButton");
    goBtn->setCursor(Qt::PointingHandCursor);

    navRow->addWidget(backBtn);
    navRow->addWidget(forwardBtn);
    navRow->addWidget(refreshBtn);
    navRow->addWidget(siteBar, 1);
    navRow->addWidget(goBtn);
    root->addLayout(navRow);

    // --- Bottom row: the search query ---
    queryBar = new QLineEdit();
    queryBar->setObjectName("searchQueryBar");
    queryBar->setPlaceholderText("Search YouTube…");
    queryBar->setClearButtonEnabled(true);
    root->addWidget(queryBar);

    // --- Results area: a grid of cards that grow to fill the row width ---
    resultsArea = new QScrollArea();
    resultsArea->setWidgetResizable(true);
    resultsArea->setFrameShape(QFrame::NoFrame);
    resultsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Always-on vertical so the scrollbar can't flicker on/off and oscillate the
    // grid width (which would re-flow on a loop).
    resultsArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Opaque, themed viewport so the grid repaints cleanly when cards reflow on a
    // resize (otherwise the gaps they vacate smear).
    resultsArea->viewport()->setAutoFillBackground(true);
    resultsArea->viewport()->setBackgroundRole(QPalette::Window);

    // Card size from Settings → Display is a target/min width; cards grow to fill.
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_targetWidth = qBound(120, cfg.value("Display/CardWidth", 240).toInt(), 480);
    m_cardWidth = m_targetWidth;

    resultsHost = new QWidget();
    resultsFlow = new FlowLayout(resultsHost, 0, kGridSpacing, kGridSpacing);
    resultsArea->setWidget(resultsHost);
    root->addWidget(resultsArea, 1);

    // Refit the cards whenever the visible width changes (window/splitter resize).
    resultsArea->viewport()->installEventFilter(this);

    // Status pill: a rounded chip holding a message + the animated seagull. Used
    // both for the initial fetch state and the "loading more results" indicator.
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
    statusPill->setObjectName("searchStatusPill"); // themed via Theme::apply's global sheet
    auto* pillInner = new QHBoxLayout(statusPill);
    pillInner->setContentsMargins(16, 7, 16, 7);
    pillInner->setSpacing(8);
    pillInner->addWidget(statusLabel);
    pillInner->addWidget(statusSpinner);

    auto* statusRow = new QHBoxLayout();
    statusRow->addStretch(1);
    statusRow->addWidget(statusPill);
    statusRow->addStretch(1);
    root->addLayout(statusRow);

    // Go button and Enter (in either bar) kick off a search.
    connect(goBtn, &QPushButton::clicked, this, &Search::performSearch);
    connect(queryBar, &QLineEdit::returnPressed, this, &Search::performSearch);
    connect(siteBar, &QLineEdit::returnPressed, this, [this]() { queryBar->setFocus(); });

    // Scroll to (near) the bottom reveals the next batch.
    connect(resultsArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar* sb = resultsArea->verticalScrollBar();
        if (sb->maximum() > 0 && value >= sb->maximum() - 48)
            loadMore();
        });

    if (m_search) {
        connect(m_search, &SgSearch::resultsReady, this, &Search::onResultsReady);
        connect(m_search, &SgSearch::failed, this, &Search::onSearchFailed);
    }
}

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
}

void Search::setStatus(const QString& text, bool busy) {
    statusLabel->setText(text);
    statusPill->setVisible(!text.isEmpty()); // empty text hides the whole pill
    if (busy) { statusSpinner->show(); statusMovie->start(); }
    else      { statusMovie->stop(); statusSpinner->hide(); }
}

void Search::addCard(const SearchResult& result) {
    auto* card = new VideoCard(result, m_nam, m_cardWidth, resultsHost);
    connect(card, &VideoCard::playRequested, this, [this](const QUrl& url, const QString& title) {
        emit playMediaRequested(url, QUrl(), QUrl(), title);
        });
    connect(card, &VideoCard::queueRequested, this, &Search::enqueueRequested);
    connect(card, &VideoCard::downloadRequested, this, &Search::downloadRequested);
    resultsFlow->addWidget(card);
}

int Search::fillCardWidth() const {
    const int vw = resultsArea->viewport()->width();
    const int target = qMax(120, m_targetWidth);
    if (vw <= 0) return target;
    // How many columns of at least the target width fit, then grow them to fill.
    const int cols = qMax(1, (vw + kGridSpacing) / (target + kGridSpacing));
    return (vw - (cols - 1) * kGridSpacing) / cols;
}

void Search::applyCardWidth() {
    const int w = fillCardWidth();
    if (w == m_cardWidth && !resultsFlow->isEmpty()) return; // nothing to do
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

    clearResults();
    m_currentQuery = query;
    m_shownCount = 0;
    m_loadingMore = false;
    m_endReached = false;

    // Snapshot the batch size at search start so changing it mid-search can't
    // corrupt the append/dedupe math of the run in flight.
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_batchSize = qBound(5, settings.value("Search/ResultLimit", 20).toInt(), 100);

    m_lastRequested = m_batchSize;
    setStatus("Fetching results.", true);
    m_search->search(SgSearch::Site::YouTube, query, m_lastRequested);
}

void Search::loadMore() {
    if (m_loadingMore || m_endReached || m_shownCount == 0 || !m_search) return;
    if (m_shownCount >= kMaxResults) { m_endReached = true; return; }

    m_loadingMore = true;
    // No continuation token from yt-dlp, so re-request the prefix + one more batch
    // and append only the new tail. Capped so the re-fetch stays cheap.
    m_lastRequested = qMin(m_shownCount + m_batchSize, kMaxResults);
    setStatus("Loading more results", true); // pill pops up at the bottom
    m_search->search(SgSearch::Site::YouTube, m_currentQuery, m_lastRequested);
}

void Search::onResultsReady(const QList<SearchResult>& results) {
    m_loadingMore = false;

    // Fewer than asked for (or nothing new) means YouTube has no more to give.
    if (results.size() < m_lastRequested || results.size() <= m_shownCount)
        m_endReached = true;

    if (m_shownCount == 0 && results.isEmpty()) {
        setStatus("No results.", false);
        return;
    }

    // Append only the new tail; everything before m_shownCount is already on screen.
    for (int i = m_shownCount; i < results.size(); ++i)
        addCard(results[i]);
    m_shownCount = results.size();

    if (m_shownCount >= kMaxResults) m_endReached = true;

    applyCardWidth();     // grow the freshly added cards to fill the row
    setStatus("", false); // hide the pill — cards are showing
}

void Search::onSearchFailed(const QString& message) {
    m_loadingMore = false;
    if (m_shownCount > 0) {
        // A "load more" failed: keep what's shown, stop the pill, and don't retry
        // on every further scroll.
        m_endReached = true;
        setStatus("", false);
    }
    else {
        clearResults();
        setStatus(message, false);
    }
}
