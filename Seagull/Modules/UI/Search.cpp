#include "Search.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "../Backend/SgSearch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QLabel>
#include <QWidget>
#include <QNetworkAccessManager>
#include <QMovie>

Search::Search(SgSearch* searchWorker, QWidget* parent)
    : QWidget(parent), m_search(searchWorker) {
    m_nam = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // --- Top row: nav buttons + site bar + Go ---
    auto* navRow = new QHBoxLayout();
    navRow->setSpacing(6);

    // Glyph buttons for now; swapped for themed SVGs once they're wired up. Left
    // inert this pass (back/forward will page cached searches, refresh re-runs).
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

    // --- Results area: a reflowing grid of cards ---
    resultsArea = new QScrollArea();
    resultsArea->setWidgetResizable(true);
    resultsArea->setFrameShape(QFrame::NoFrame);

    resultsHost = new QWidget();
    resultsFlow = new FlowLayout(resultsHost, 0, 12, 12);
    resultsArea->setWidget(resultsHost);
    root->addWidget(resultsArea, 1);

    // Status row: a message with the animated seagull beside it while searching.
    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(8);

    statusLabel = new QLabel("Search YouTube to see results.");
    statusLabel->setObjectName("searchStatus");

    statusMovie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), this);
    statusMovie->jumpToFrame(0);
    const QSize frame = statusMovie->currentPixmap().size();
    const int spinH = 24;
    const int spinW = frame.height() > 0 ? frame.width() * spinH / frame.height() : spinH;
    statusMovie->setScaledSize(QSize(spinW, spinH));
    statusSpinner = new QLabel();
    statusSpinner->setMovie(statusMovie);
    statusSpinner->hide();

    statusRow->addStretch(1);
    statusRow->addWidget(statusLabel);
    statusRow->addWidget(statusSpinner);
    statusRow->addStretch(1);
    root->addLayout(statusRow);

    // Go button and Enter (in either bar) kick off a search.
    connect(goBtn, &QPushButton::clicked, this, &Search::performSearch);
    connect(queryBar, &QLineEdit::returnPressed, this, &Search::performSearch);
    connect(siteBar, &QLineEdit::returnPressed, this, [this]() { queryBar->setFocus(); });

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
    statusLabel->setVisible(!text.isEmpty());
    if (busy) { statusSpinner->show(); statusMovie->start(); }
    else      { statusMovie->stop(); statusSpinner->hide(); }
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
    setStatus("Fetching results.", true);
    m_search->search(SgSearch::Site::YouTube, query);
}

void Search::onResultsReady(const QList<SearchResult>& results) {
    clearResults();
    if (results.isEmpty()) { setStatus("No results.", false); return; }

    setStatus("", false);
    for (const SearchResult& r : results) {
        auto* card = new VideoCard(r, m_nam, resultsHost);
        connect(card, &VideoCard::playRequested, this, [this](const QUrl& url, const QString& title) {
            emit playMediaRequested(url, QUrl(), QUrl(), title);
            });
        connect(card, &VideoCard::queueRequested, this, &Search::enqueueRequested);
        connect(card, &VideoCard::downloadRequested, this, &Search::downloadRequested);
        resultsFlow->addWidget(card);
    }
}

void Search::onSearchFailed(const QString& message) {
    clearResults();
    setStatus(message, false);
}
