#include "Search.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "Widgets/SpellCheckLineEdit.h"
#include "../Backend/SgSearch.h"
#include "../Backend/SgThumbnailer.h" // decodeViaFfmpeg (WebP avatars)

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QLabel>
#include <QWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
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
#include <QFile>
#include <QTextStream>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QPixmap>
#include <QPalette>
#include <algorithm>

namespace {
constexpr int kGridSpacing = 12;
constexpr int kSearchGraceMs = 600; // keep the filter bar up briefly after the magnifier click
}

Search::Search(SgSearch* searchWorker, SgSpellCheck* spell, QWidget* parent)
    : QWidget(parent), m_search(searchWorker), m_spell(spell) {
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

    siteBar = new QComboBox();
    siteBar->setObjectName("searchSiteBar");
    siteBar->setEditable(true);                      // type a site OR pick from the dropdown
    siteBar->setInsertPolicy(QComboBox::NoInsert);   // typing a query never adds junk items
    siteBar->addItems({ "YouTube", "PornHub" });     // the sites yt-dlp can search for us
    siteBar->setCurrentText("YouTube");
    siteBar->lineEdit()->setPlaceholderText("Site \xe2\x80\x94 e.g. youtube");
    siteBar->setToolTip("Type or pick a site to search.");

    goBtn = new QPushButton(QStringLiteral("Go"));
    goBtn->setObjectName("searchGoButton");
    goBtn->setCursor(Qt::PointingHandCursor);

    navRow->addWidget(backBtn);
    navRow->addWidget(forwardBtn);
    navRow->addWidget(refreshBtn);
    navRow->addWidget(siteBar, 1);
    navRow->addWidget(goBtn);
    chromeLay->addLayout(navRow);

    // Query bar: an editable combo like the File Explorer's address bar — the
    // arrow drops the full search history, typing filters it via the completer.
    queryBar = new QComboBox();
    queryBar->setObjectName("searchQueryBar");
    queryBar->setEditable(true);
    // Swap the combo's inner editor for a spell-checking line edit (red squiggle
    // + right-click suggestions). The combo keeps owning the history model,
    // dropdown, and completer — only the text field is replaced. Must run before
    // the lineEdit() config below, which setLineEdit would otherwise discard.
    queryBar->setLineEdit(new SpellCheckLineEdit(m_spell, queryBar));
    queryBar->setInsertPolicy(QComboBox::NoInsert); // we manage the items (addToHistory)
    queryBar->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    queryBar->lineEdit()->setPlaceholderText("Search YouTube\xe2\x80\xa6");
    queryBar->lineEdit()->setClearButtonEnabled(true);

    m_historyModel     = new QStringListModel(this);
    m_historyCompleter = new QCompleter(m_historyModel, this);
    // Search-engine style: the history drops down while typing, filtered to
    // entries containing what's typed (not the whole list regardless of text).
    m_historyCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_historyCompleter->setFilterMode(Qt::MatchContains);
    m_historyCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    queryBar->lineEdit()->setCompleter(m_historyCompleter);
    chromeLay->addWidget(queryBar);

    loadHistory(); // history persists across sessions (plain-text file)
    queryBar->setCurrentIndex(-1); // start with an empty bar, not the newest entry

    root->addWidget(chromeWidget);

    // --- Full-width separator (browser chrome / page boundary) ---
    auto* sep = new QFrame();
    sep->setObjectName("searchSeparator");
    sep->setFixedHeight(1);
    root->addWidget(sep);

    // --- Channel header (avatar + name + subscribers) ---
    // Shown above the grid only in channel view; back/forward leave it.
    m_channelHeader = new QFrame();
    m_channelHeader->setObjectName("channelHeader");
    auto* chLay = new QHBoxLayout(m_channelHeader);
    chLay->setContentsMargins(14, 10, 14, 10);
    chLay->setSpacing(12);
    m_channelAvatar = new QLabel(m_channelHeader);
    m_channelAvatar->setFixedSize(56, 56);
    m_channelAvatar->setScaledContents(false);
    chLay->addWidget(m_channelAvatar);
    auto* chText = new QVBoxLayout();
    chText->setSpacing(2);
    m_channelName = new QLabel(m_channelHeader);
    m_channelName->setObjectName("channelHeaderName");
    QFont chf = m_channelName->font();
    chf.setBold(true);
    chf.setPointSizeF(chf.pointSizeF() + 3);
    m_channelName->setFont(chf);
    m_channelSubs = new QLabel(m_channelHeader);
    m_channelSubs->setObjectName("metaStats");
    chText->addWidget(m_channelName);
    chText->addWidget(m_channelSubs);
    chLay->addLayout(chText);
    chLay->addStretch(1);
    m_channelHeader->hide();
    root->addWidget(m_channelHeader);

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
    m_targetWidth = qBound(120, cfg.value("Display/CardWidth", 360).toInt(), 480); // default Extra Large
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
    m_filterVideosBtn->setChecked(true); // always launch on Videos

    // --- Top-right magnifier + sort chips (mirroring the Library tab) ---
    // Children of resultsArea, like the pill, so raise() keeps them above the
    // viewport. Reuse the Library object names so the theme styles them identically.
    m_resultSearchBtn = new QPushButton(resultsArea);
    m_resultSearchBtn->setObjectName("librarySearchButton");
    m_resultSearchBtn->setCursor(Qt::PointingHandCursor);
    m_resultSearchBtn->setFixedSize(34, 34);
    m_resultSearchBtn->setToolTip("Filter these results");
    connect(m_resultSearchBtn, &QPushButton::clicked, this, &Search::toggleResultSearch);

    m_resultSearchBar = new SpellCheckLineEdit(m_spell, resultsArea);
    m_resultSearchBar->setObjectName("librarySearchBar");
    m_resultSearchBar->setPlaceholderText("Filter\xE2\x80\xA6");
    m_resultSearchBar->setClearButtonEnabled(true);
    m_resultSearchBar->setFixedWidth(180);
    m_resultSearchBar->hide();
    connect(m_resultSearchBar, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_resultQuery = t;
        rebuildCards();
    });

    m_resultSortBtn = new QPushButton(resultsArea);
    m_resultSortBtn->setObjectName("librarySearchButton");
    m_resultSortBtn->setCursor(Qt::PointingHandCursor);
    m_resultSortBtn->setFixedSize(34, 34);
    m_resultSortBtn->setToolTip("Sort these results");
    connect(m_resultSortBtn, &QPushButton::clicked, this, &Search::showResultSortMenu);
    tintResultSearchIcon();
    tintResultSortIcon();

    // Persisted ordering (default Newest first), loaded before the menu so the
    // right item starts checked.
    {
        QSettings sortCfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
        m_sortMode = static_cast<SortMode>(qBound(0,
            sortCfg.value("Search/SortMode", static_cast<int>(SortMode::Newest)).toInt(),
            static_cast<int>(SortMode::Oldest)));
    }

    m_resultSortMenu = new QMenu(this);
    auto* sortGroup = new QActionGroup(m_resultSortMenu);
    sortGroup->setExclusive(true);
    const struct { const char* label; SortMode mode; } orderings[] = {
        { "Relevance",              SortMode::Relevance },
        { "Name (A\xE2\x80\x93" "Z)", SortMode::NameAsc },
        { "Name (Z\xE2\x80\x93" "A)", SortMode::NameDesc },
        { "Newest",                 SortMode::Newest },
        { "Oldest",                 SortMode::Oldest },
    };
    for (const auto& o : orderings) {
        QAction* a = m_resultSortMenu->addAction(QString::fromUtf8(o.label));
        a->setCheckable(true);
        a->setChecked(o.mode == m_sortMode);
        sortGroup->addAction(a);
        const SortMode mode = o.mode;
        connect(a, &QAction::triggered, this, [this, mode] { applySortMode(mode); });
    }

    // Reserve top space in the flow layout so the first card row clears the pill and
    // the (taller) chips. Done after sizeHint() is meaningful (pill is laid out).
    m_filterPill->adjustSize();
    const int pillH = qMax(m_filterPill->sizeHint().height(), m_resultSearchBtn->height());
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
    connect(goBtn,    &QPushButton::clicked, this, &Search::performSearch);
    connect(queryBar->lineEdit(), &QLineEdit::returnPressed, this, &Search::performSearch);
    // Picking a history entry from the arrow dropdown searches it right away.
    connect(queryBar, &QComboBox::textActivated, this, &Search::performSearch);
    // Switching site updates the query bar's prompt to name the site being searched.
    connect(siteBar, &QComboBox::currentTextChanged, this, [this](const QString&) { updateQueryPlaceholder(); });
    // Enter in the site box jumps to the query so you can just type and search.
    connect(siteBar->lineEdit(), &QLineEdit::returnPressed, this, [this]() { queryBar->setFocus(); });
    updateQueryPlaceholder(); // initial prompt ("Search YouTube")

    connect(backBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex > 0) navigateTo(m_navIndex - 1);
    });

    connect(forwardBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex < m_navHistory.size() - 1) navigateTo(m_navIndex + 1);
    });

    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex < 0 || m_navIndex >= m_navHistory.size()) return;
        for (const SearchResult& r : m_allResults)
            if (!r.thumbnail.isEmpty()) QPixmapCache::remove(r.thumbnail);
        navigateTo(m_navIndex); // reload the current page (search or channel)
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
        connect(m_search, &SgSearch::resultsReady,       this, &Search::onResultsReady);
        connect(m_search, &SgSearch::channelVideosReady, this, &Search::onChannelVideosReady);
        connect(m_search, &SgSearch::failed,             this, &Search::onSearchFailed);
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
    positionTopControls(); // right-anchored chips track the new width
}

void Search::positionFilterPill() {
    m_filterPill->adjustSize();
    // Coordinates are relative to resultsArea (the pill's parent).
    m_filterPill->move(10, kPillTopMargin);
    m_filterPill->raise();
    positionTopControls();
}

void Search::positionTopControls() {
    if (!m_resultSortBtn) return;
    // Top-right, just inside the always-on vertical scrollbar (same as Library).
    QScrollBar* vsb = resultsArea->verticalScrollBar();
    const int sb = vsb->isVisible() ? vsb->width() : vsb->sizeHint().width();
    const int rightEdge = resultsArea->width() - kPillTopMargin - sb - 2;
    constexpr int gap = 6;

    // Sort at the far right; the magnifier (and its expanding bar) to its left.
    m_resultSortBtn->move(rightEdge - m_resultSortBtn->width(), kPillTopMargin);
    m_resultSortBtn->raise();

    const int searchRight = rightEdge - m_resultSortBtn->width() - gap;
    m_resultSearchBtn->move(searchRight - m_resultSearchBtn->width(), kPillTopMargin);
    m_resultSearchBtn->raise();
    m_resultSearchBar->move(searchRight - m_resultSearchBar->width(),
        kPillTopMargin + (m_resultSearchBtn->height() - m_resultSearchBar->height()) / 2);
    m_resultSearchBar->raise();
}

void Search::updateFilterPillVisibility() {
    if (m_viewMode == ViewMode::Channel) { // no chrome on a channel page
        m_filterPill->hide();
        m_resultSearchBtn->hide();
        m_resultSearchBar->hide();
        m_resultSortBtn->hide();
        return;
    }
    const bool atTop = (resultsArea->verticalScrollBar()->value() <= 0);
    // Zone is relative to resultsArea (the chips' parent); tall enough to cover the
    // chips (taller than the pill) so hovering them doesn't fall outside and hide them.
    const int stripH = qMax(m_filterPill->height(), m_resultSortBtn->height());
    const QRect zone(0, 0, resultsArea->width(), stripH + 2 * kPillTopMargin);
    const bool hovered = zone.contains(resultsArea->mapFromGlobal(QCursor::pos()));
    const bool show = atTop || hovered;
    // The Videos/Shorts pill is YouTube-only; other sites have no such split.
    m_filterPill->setVisible(show && m_currentSite == SgSearch::Site::YouTube);
    m_resultSortBtn->setVisible(show);

    // The filter bar takes the magnifier's place while it's in use (focused, holding
    // a query, or just opened). Otherwise it collapses back to the magnifier.
    if (m_resultSearchOpen) {
        const bool inUse = m_resultSearchBar->hasFocus()
                        || !m_resultQuery.trimmed().isEmpty()
                        || (m_resultSearchOpenedClock.isValid() && m_resultSearchOpenedClock.elapsed() < kSearchGraceMs);
        if (inUse) {
            m_resultSearchBtn->hide();
            m_resultSearchBar->setVisible(true);
            m_resultSearchBar->raise();
            return;
        }
        m_resultSearchOpen = false;
        m_resultSearchBar->clear(); // resets the filter so it reopens blank
    }
    m_resultSearchBar->hide();
    m_resultSearchBtn->setVisible(show);
}

void Search::toggleResultSearch() {
    m_resultSearchOpen = !m_resultSearchOpen;
    if (m_resultSearchOpen) {
        m_resultSearchOpenedClock.restart();
        positionTopControls();
        m_resultSearchBar->show();
        m_resultSearchBar->raise();
        m_resultSearchBar->setFocus(Qt::OtherFocusReason);
    } else {
        m_resultSearchBar->clear(); // drops the filter
        m_resultSearchBar->hide();
    }
    updateFilterPillVisibility();
}

void Search::tintResultSearchIcon() {
    if (!m_resultSearchBtn) return;
    const QSize sz(18, 18);
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/search.svg")).pixmap(sz);
    if (pm.isNull()) return;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), palette().color(QPalette::WindowText));
    p.end();
    m_resultSearchBtn->setIcon(QIcon(pm));
    m_resultSearchBtn->setIconSize(sz);
}

void Search::tintResultSortIcon() {
    if (!m_resultSortBtn) return;
    const QSize sz(18, 18);
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/sort.svg")).pixmap(sz);
    if (pm.isNull()) return;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), palette().color(QPalette::WindowText));
    p.end();
    m_resultSortBtn->setIcon(QIcon(pm));
    m_resultSortBtn->setIconSize(sz);
}

void Search::showResultSortMenu() {
    m_resultSortMenu->popup(m_resultSortBtn->mapToGlobal(QPoint(0, m_resultSortBtn->height())));
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

void Search::pushNavEntry(const NavEntry& entry) {
    if (m_navIndex < m_navHistory.size() - 1)
        m_navHistory = m_navHistory.mid(0, m_navIndex + 1);
    m_navHistory.append(entry);
    m_navIndex = m_navHistory.size() - 1;
    updateNavButtons();
}

void Search::navigateTo(int index) {
    if (index < 0 || index >= m_navHistory.size()) return;
    m_navIndex = index;
    const NavEntry& e = m_navHistory[index];
    queryBar->setCurrentText(e.label);
    if (e.kind == NavEntry::Channel) {
        openChannelUrl(e.target, e.label);
    } else {
        // Restore the site this query ran against so startSearch routes correctly.
        siteBar->blockSignals(true);
        siteBar->setCurrentText(e.site == SgSearch::Site::PornHub ? "PornHub" : "YouTube");
        siteBar->blockSignals(false);
        startSearch(e.target);
    }
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

    // Mirror into the combo's item list incrementally (a full rebuild would
    // disturb the edit text mid-search).
    queryBar->blockSignals(true);
    const int dup = queryBar->findText(query, Qt::MatchFixedString);
    if (dup >= 0) queryBar->removeItem(dup);
    queryBar->insertItem(0, query);
    while (queryBar->count() > 50) queryBar->removeItem(queryBar->count() - 1);
    queryBar->setCurrentIndex(0); // the bar keeps showing the running query
    queryBar->blockSignals(false);

    saveHistory();
}

// History lives in a plain-text file the user can open and read: one query
// per line, most recent first, next to config.ini.
QString Search::historyFilePath() {
    return QCoreApplication::applicationDirPath() + "/search_history.txt";
}

void Search::loadHistory() {
    QFile f(historyFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return; // none yet
    m_searchHistory.clear();
    QTextStream in(&f);
    while (!in.atEnd() && m_searchHistory.size() < 50) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty()) m_searchHistory.append(line);
    }
    m_historyModel->setStringList(m_searchHistory);
    queryBar->blockSignals(true);
    queryBar->clear();
    queryBar->addItems(m_searchHistory);
    queryBar->setCurrentIndex(-1);
    queryBar->blockSignals(false);
}

void Search::saveHistory() {
    QFile f(historyFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream out(&f);
    for (const QString& q : m_searchHistory) out << q << '\n';
}

void Search::clearSearchHistory() {
    m_searchHistory.clear();
    m_historyModel->setStringList(m_searchHistory);
    queryBar->blockSignals(true);
    queryBar->clear(); // items and edit text both go
    queryBar->blockSignals(false);
    QFile::remove(historyFilePath());
}

// ---------------------------------------------------------------------------
// Search execution
// ---------------------------------------------------------------------------

void Search::performSearch() {
    // Enter on a query that matches a history item fires returnPressed AND
    // textActivated back-to-back; collapse the pair into one search.
    if (m_searchFiring) return;
    m_searchFiring = true;
    QTimer::singleShot(0, this, [this]() { m_searchFiring = false; });

    const QString query = queryBar->currentText().trimmed();

    if (query.isEmpty()) {
        setStatus("Enter something to search for.", false);
        return;
    }
    if (!m_search) {
        setStatus("Search backend unavailable.", false);
        return;
    }

    pushNavEntry({ NavEntry::Query, query, query, currentSite() });
    addToHistory(query);
    startSearch(query);
}

void Search::startSearch(const QString& query) {
    setViewMode(ViewMode::Search);
    clearResults();
    m_currentQuery = query;
    m_currentSite  = currentSite(); // remember the site for paging (loadMore)
    m_shownCount   = 0;
    m_loadingMore  = false;
    m_endReached   = false;
    refreshBtn->setEnabled(true);

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_batchSize = qBound(5, settings.value("Search/ResultLimit", 20).toInt(), 100);
    m_lastRequested = m_batchSize;

    // "channel:" prefix -> search for channels (their own cards) via the internal
    // API; yt-dlp can't list channels by name. YouTube only; one page, no load-more.
    const QString trimmed = query.trimmed();
    if (m_currentSite == SgSearch::Site::YouTube
        && trimmed.startsWith("channel:", Qt::CaseInsensitive)) {
        const QString name = trimmed.mid(QStringLiteral("channel:").size()).trimmed();
        if (name.isEmpty()) { setStatus("Type a channel name after \"channel:\".", false); return; }
        m_endReached = true; // channel search is a single page
        setStatus("Searching channels.", true);
        m_search->searchChannels(name, m_lastRequested);
        return;
    }

    setStatus("Fetching results.", true);
    // Shorts is a YouTube-only mode; other sites always search plain videos.
    const bool shorts = (m_currentSite == SgSearch::Site::YouTube && m_filterMode == FilterMode::Shorts);
    m_search->search(m_currentSite, query, m_lastRequested, shorts);
}

void Search::loadMore() {
    if (m_loadingMore || m_endReached || m_shownCount == 0 || !m_search) return;
    if (m_shownCount >= kMaxResults) { m_endReached = true; return; }

    m_loadingMore = true;
    m_lastRequested = qMin(m_shownCount + m_batchSize, kMaxResults);
    if (m_viewMode == ViewMode::Channel) {
        setStatus("Loading more videos", true);
        m_search->fetchChannelVideos(m_currentChannelUrl, m_lastRequested);
    } else {
        setStatus("Loading more results", true);
        const bool shorts = (m_currentSite == SgSearch::Site::YouTube && m_filterMode == FilterMode::Shorts);
        m_search->search(m_currentSite, m_currentQuery, m_lastRequested, shorts);
    }
}

// ---------------------------------------------------------------------------
// Feed advance (shorts wheel / skip buttons)
// ---------------------------------------------------------------------------

void Search::playAdjacentResult(int delta) {
    if (m_allResults.isEmpty() || m_playingIndex < 0) return;

    const int step = (delta > 0) ? 1 : -1;
    int i = m_playingIndex + step;
    while (i >= 0 && i < m_allResults.size() && !shows(m_allResults[i]))
        i += step;

    if (i < 0) return; // top of the feed — nothing before the first result
    if (i >= m_allResults.size()) {
        // Ran off the loaded tail — pull the next batch and continue when it
        // lands (onResultsReady resumes the advance).
        if (!m_endReached) {
            m_advancePending = true;
            if (!m_loadingMore) loadMore();
        }
        return;
    }
    playResultAt(i);
}

void Search::playResultAt(int index) {
    m_playingIndex = index;
    const SearchResult& r = m_allResults[index];
    emit playMediaRequested(QUrl(r.url), QUrl(), QUrl(), r.title);

    // Keep the grid tracking the feed: the card's flow position is its rank
    // among the filtered results.
    int cardPos = 0;
    for (int j = 0; j < index; ++j)
        if (shows(m_allResults[j])) ++cardPos;
    if (QLayoutItem* it = resultsFlow->itemAt(cardPos))
        if (QWidget* w = it->widget()) resultsArea->ensureWidgetVisible(w);
}

// ---------------------------------------------------------------------------
// Result handling
// ---------------------------------------------------------------------------

void Search::onResultsReady(const QList<SearchResult>& results) {
    if (m_viewMode != ViewMode::Search) return; // a late search reply after opening a channel
    ingestResults(results);
}

void Search::onChannelVideosReady(const SearchResult& channelInfo,
                                  const QList<SearchResult>& videos) {
    if (m_viewMode != ViewMode::Channel) return; // late reply after navigating away
    if (m_shownCount == 0) updateChannelHeader(channelInfo); // first page fills the header
    ingestResults(videos);
}

void Search::ingestResults(const QList<SearchResult>& results) {
    m_loadingMore = false;

    if (results.size() < m_lastRequested || results.size() <= m_shownCount)
        m_endReached = true;

    if (m_shownCount == 0 && results.isEmpty()) {
        setStatus("No results.", false);
        return;
    }

    const int before = m_allResults.size();
    for (int i = m_shownCount; i < results.size(); ++i) {
        // YouTube search returns the same video in multiple entries (shelves,
        // re-paged results); skip anything whose URL we've already shown.
        const QString& url = results[i].url;
        if (!url.isEmpty()) {
            if (m_seenUrls.contains(url)) continue;
            m_seenUrls.insert(url);
        }
        SearchResult r = results[i];
        r.seq = m_seqCounter++; // arrival rank (the "Relevance" order)
        m_allResults.append(r);
    }
    m_shownCount = results.size();

    // Channel pages show their natural order (the sort chips are hidden there).
    if (m_viewMode == ViewMode::Search && m_sortMode != SortMode::Relevance) {
        // A non-default sort means the new arrivals have to merge into the order, so
        // re-sort and rebuild the grid wholesale.
        applySort();
        rebuildCards();
    } else {
        // Relevance: the new arrivals belong at the end — just append their cards.
        for (int i = before; i < m_allResults.size(); ++i)
            if (shows(m_allResults[i])) addCard(m_allResults[i]);
    }

    if (m_shownCount >= kMaxResults) m_endReached = true;

    applyCardWidth();
    setStatus("", false);

    // A feed advance was waiting on this batch — keep walking now that it's here.
    if (m_advancePending) {
        m_advancePending = false;
        playAdjacentResult(1);
    }

    // If filtered results don't fill the viewport yet, keep loading automatically.
    if (!m_endReached) {
        QScrollBar* sb = resultsArea->verticalScrollBar();
        if (sb->maximum() == 0) loadMore();
    }
}

void Search::onSearchFailed(const QString& message) {
    m_loadingMore = false;
    m_advancePending = false;
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
    if (m_viewMode == ViewMode::Channel) return true; // a channel page shows all its videos
    if (m_currentSite != SgSearch::Site::YouTube) return true; // no Videos/Shorts split off YouTube
    if (r.isChannel) return true; // channel cards aren't videos/shorts; never filtered out
    switch (m_filterMode) {
    case FilterMode::Videos:  return !r.isShort;
    case FilterMode::Shorts:  return r.isShort;
    default:                  return true;
    }
}

bool Search::matchesQuery(const SearchResult& r) const {
    const QString q = m_resultQuery.trimmed();
    if (q.isEmpty()) return true;
    return r.title.contains(q, Qt::CaseInsensitive)
        || r.channel.contains(q, Qt::CaseInsensitive);
}

bool Search::shows(const SearchResult& r) const {
    return passesFilter(r) && matchesQuery(r);
}

void Search::applySortMode(SortMode mode) {
    if (mode == m_sortMode) return;
    m_sortMode = mode;
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    cfg.setValue("Search/SortMode", static_cast<int>(mode));
    applySort();
    rebuildCards();
}

void Search::applySort() {
    // Keep the playing item under m_playingIndex after the reorder.
    const QString playingUrl = (m_playingIndex >= 0 && m_playingIndex < m_allResults.size())
        ? m_allResults[m_playingIndex].url : QString();

    auto& v = m_allResults;
    switch (m_sortMode) {
    case SortMode::Relevance:
        std::stable_sort(v.begin(), v.end(),
            [](const SearchResult& a, const SearchResult& b) { return a.seq < b.seq; });
        break;
    case SortMode::NameAsc:
        std::stable_sort(v.begin(), v.end(), [](const SearchResult& a, const SearchResult& b) {
            const int c = a.title.compare(b.title, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : a.seq < b.seq;
        });
        break;
    case SortMode::NameDesc:
        std::stable_sort(v.begin(), v.end(), [](const SearchResult& a, const SearchResult& b) {
            const int c = a.title.compare(b.title, Qt::CaseInsensitive);
            return c != 0 ? c > 0 : a.seq < b.seq;
        });
        break;
    case SortMode::Newest:
    case SortMode::Oldest: {
        const bool newest = (m_sortMode == SortMode::Newest);
        std::stable_sort(v.begin(), v.end(), [newest](const SearchResult& a, const SearchResult& b) {
            const bool ka = a.timestamp >= 0, kb = b.timestamp >= 0;
            if (ka != kb) return ka;                 // dated results sort ahead of undated
            if (!ka)      return a.seq < b.seq;       // both undated: keep relevance order
            if (a.timestamp != b.timestamp)
                return newest ? a.timestamp > b.timestamp : a.timestamp < b.timestamp;
            return a.seq < b.seq;
        });
        break;
    }
    }

    if (!playingUrl.isEmpty()) {
        m_playingIndex = -1;
        for (int i = 0; i < v.size(); ++i)
            if (v[i].url == playingUrl) { m_playingIndex = i; break; }
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
    // query — but ONLY the query still sitting in the bar. A cleared or
    // edited bar means the user is lining up a different search; re-running
    // the old term would burn a YouTube request they never asked for.
    if (crossesShorts) {
        const QString barText = queryBar->currentText().trimmed();
        if (!barText.isEmpty() && barText == m_currentQuery) {
            startSearch(m_currentQuery);
        } else {
            m_endReached = true; // freeze paging on the stale results too
            rebuildCards();      // typically empties the grid until they search
        }
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
        if (shows(r)) addCard(r);
    applyCardWidth();
}

// ---------------------------------------------------------------------------
// Channel pages (reached / left with the back/forward buttons)
// ---------------------------------------------------------------------------

void Search::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    m_channelHeader->setVisible(mode == ViewMode::Channel);
    updateFilterPillVisibility(); // hides the Videos/Shorts pill in channel view
}

void Search::openChannel(const QString& channelUrl, const QString& name) {
    if (channelUrl.trimmed().isEmpty()) return;
    pushNavEntry({ NavEntry::Channel, channelUrl, name });
    openChannelUrl(channelUrl, name);
}

void Search::openChannelUrl(const QString& channelUrl, const QString& label) {
    if (!m_search) return;
    setViewMode(ViewMode::Channel);
    queryBar->setCurrentText(label); // address-bar shows where you are
    clearResults();
    m_currentChannelUrl = channelUrl;
    m_shownCount  = 0;
    m_loadingMore = false;
    m_endReached  = false;
    refreshBtn->setEnabled(true);

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_batchSize = qBound(5, settings.value("Search/ResultLimit", 20).toInt(), 100);
    m_lastRequested = m_batchSize;

    // Provisional header; the real name/subs/avatar arrive with the first batch.
    m_channelName->setText(label);
    m_channelSubs->clear();
    loadAvatar(QString()); // MDI glyph until the avatar lands
    resultsArea->verticalScrollBar()->setValue(0);

    setStatus("Loading channel.", true);
    m_search->fetchChannelVideos(channelUrl, m_lastRequested);
}

void Search::updateChannelHeader(const SearchResult& info) {
    if (!info.title.isEmpty()) m_channelName->setText(info.title);
    m_channelSubs->setText(info.subscriberCount >= 0
        ? VideoCard::formatViewCount(info.subscriberCount) + " subscribers"
        : QString());
    if (!info.thumbnail.isEmpty()) loadAvatar(info.thumbnail);
}

void Search::loadAvatar(const QString& url) {
    const int d = m_channelAvatar->width();
    auto setGlyph = [this, d]() {
        QPixmap pm(d, d);
        pm.fill(Qt::transparent);
        QSvgRenderer r(QStringLiteral(":/Assets/icons/account.svg"));
        QPainter p(&pm);
        const int g = d * 3 / 4;
        r.render(&p, QRectF((d - g) / 2.0, (d - g) / 2.0, g, g));
        p.end();
        m_channelAvatar->setPixmap(pm);
    };
    auto setCircular = [this, d](const QPixmap& src) {
        QPixmap scaled = src.scaled(d, d, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        QPixmap out(d, d);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath path;
        path.addEllipse(0, 0, d, d);
        p.setClipPath(path);
        p.drawPixmap((d - scaled.width()) / 2, (d - scaled.height()) / 2, scaled);
        p.end();
        m_channelAvatar->setPixmap(out);
    };

    if (url.isEmpty()) { setGlyph(); return; }

    QNetworkRequest req((QUrl(url)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, setGlyph, setCircular]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { setGlyph(); return; }
        const QByteArray data = reply->readAll();
        QPixmap pm;
        if (pm.loadFromData(data)) { setCircular(pm); return; }
        // WebP avatar (this Qt has no qwebp plugin) -> decode via ffmpeg.
        SgThumbnailer::decodeViaFfmpeg(data, this, [setGlyph, setCircular](const QPixmap& dec) {
            if (dec.isNull()) setGlyph(); else setCircular(dec);
        });
    });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

SgSearch::Site Search::currentSite() const {
    const QString s = siteBar->currentText().trimmed().toLower();
    return (s.contains("porn") || s == "ph") ? SgSearch::Site::PornHub
                                             : SgSearch::Site::YouTube;
}

void Search::updateQueryPlaceholder() {
    const QString site = (currentSite() == SgSearch::Site::PornHub) ? "PornHub" : "YouTube";
    if (queryBar->lineEdit())
        queryBar->lineEdit()->setPlaceholderText("Search " + site);
}

void Search::clearResults() {
    QLayoutItem* item;
    while ((item = resultsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_allResults.clear();
    m_seenUrls.clear();
    m_playingIndex = -1;
    m_advancePending = false;
    m_seqCounter = 0;

    // Drop the title filter for the fresh result set (without firing a rebuild).
    m_resultQuery.clear();
    m_resultSearchOpen = false;
    if (m_resultSearchBar) {
        m_resultSearchBar->blockSignals(true);
        m_resultSearchBar->clear();
        m_resultSearchBar->blockSignals(false);
        m_resultSearchBar->hide();
    }
}

void Search::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        tintResultSearchIcon();
        tintResultSortIcon();
    }
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
        // Remember the feed position so wheel/skip can walk from here.
        m_playingIndex = -1;
        for (int i = 0; i < m_allResults.size(); ++i)
            if (m_allResults[i].url == url.toString()) { m_playingIndex = i; break; }
        emit playMediaRequested(url, QUrl(), QUrl(), title);
    });
    connect(card, &VideoCard::queueRequested,    this, &Search::enqueueRequested);
    connect(card, &VideoCard::downloadRequested, this, &Search::downloadRequested);
    // Uploader link (video cards) or "View Channel" (channel cards) -> open the
    // channel's video page as a browser-style navigation.
    connect(card, &VideoCard::channelRequested,  this, &Search::openChannel);
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
    if (obj == resultsArea->viewport() && event->type() == QEvent::Resize) {
        applyCardWidth();
        positionTopControls(); // the scrollbar's presence shifts the right edge
    }
    return QWidget::eventFilter(obj, event);
}
