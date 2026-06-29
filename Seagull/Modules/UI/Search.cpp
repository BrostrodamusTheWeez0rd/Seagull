#include "Search.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "Widgets/SpellCheckLineEdit.h"
#include "../Backend/SgSearch.h"
#include "../Backend/SgFavorites.h"
#include "../Backend/SgThumbnailer.h" // decodeViaFfmpeg (WebP avatars)
#include "../Backend/SgPaths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QRandomGenerator>
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
#include <QDateTime>
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
#include <QMessageBox>
#include <QCheckBox>
#include <QHash>
#include <algorithm>

namespace {
constexpr int kGridSpacing = 12;
constexpr int kSearchGraceMs = 600; // keep the filter bar up briefly after the magnifier click

// Per-site suffix for the home-page config keys, e.g. "Search/HomeChannels" + suffix
// (the ranked picker) and "Search/HomeAmount" + suffix (how many to show). Kept in one
// place so Settings and Search always agree on the key names.
QString homeSiteSuffix(SgSearch::Site site) {
    switch (site) {
    case SgSearch::Site::PornHub:    return QStringLiteral("PornHub");
    case SgSearch::Site::Chaturbate: return QStringLiteral("Chaturbate");
    default:                         return QStringLiteral("YouTube");
    }
}
}

QList<Search*> Search::s_instances; // every live Search tab (GUI thread only)

Search::Search(SgSearch* searchWorker, SgSpellCheck* spell, QWidget* parent)
    : QWidget(parent), m_search(searchWorker), m_spell(spell) {
    s_instances.append(this);
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
    homeBtn    = new QPushButton(QStringLiteral("⌂"));
    for (QPushButton* b : { backBtn, forwardBtn, refreshBtn, homeBtn }) {
        b->setObjectName("searchNavButton");
        b->setFixedWidth(34);
        b->setCursor(Qt::PointingHandCursor);
    }
    backBtn->setEnabled(false);
    forwardBtn->setEnabled(false);
    refreshBtn->setEnabled(false);
    homeBtn->setToolTip("Home");

    siteBar = new QComboBox();
    siteBar->setObjectName("searchSiteBar");
    siteBar->setEditable(true);                      // type a site OR pick from the dropdown
    siteBar->setInsertPolicy(QComboBox::NoInsert);   // typing a query never adds junk items
    siteBar->addItems({ "YouTube", "PornHub", "Chaturbate" }); // searchable sites
    siteBar->setCurrentText("YouTube");
    siteBar->lineEdit()->setPlaceholderText("Site \xe2\x80\x94 e.g. youtube");
    siteBar->setToolTip("Type or pick a site to search.");

    goBtn = new QPushButton(QStringLiteral("Go"));
    goBtn->setObjectName("searchGoButton");
    goBtn->setCursor(Qt::PointingHandCursor);

    navRow->addWidget(backBtn);
    navRow->addWidget(forwardBtn);
    navRow->addWidget(refreshBtn);
    navRow->addWidget(homeBtn);
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

    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    m_targetWidth = qBound(120, cfg.value("Display/CardWidth", 300).toInt(), 480); // default Large
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

    // Favourites chip (YouTube + PornHub). Toggles a view that lists all starred
    // channels/models for the current site.
    m_favoritesBtn = new QPushButton(resultsArea);
    m_favoritesBtn->setObjectName("librarySearchButton");
    m_favoritesBtn->setCursor(Qt::PointingHandCursor);
    m_favoritesBtn->setFixedSize(34, 34);
    m_favoritesBtn->setToolTip("Show favourites");
    connect(m_favoritesBtn, &QPushButton::clicked, this, [this]() {
        if (m_favoritesActive) exitFavoritesView();
        else                   enterFavoritesView();
    });

    tintResultSearchIcon();
    tintResultSortIcon();
    tintFavoritesIcon();

    // Persisted ordering (default Newest first), loaded before the menu so the
    // right item starts checked.
    {
        QSettings sortCfg(SgPaths::configFile(), QSettings::IniFormat);
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

    statusLabel = new QLabel(emptyStateText());
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
    // Switching site swaps the per-site history + updates the prompt/idle label.
    // The favourites chip shows on favouritable sites (YouTube + PornHub).
    connect(siteBar, &QComboBox::currentTextChanged, this, [this](const QString&) {
        const SgSearch::Site s = currentSite();
        if (s == m_uiSite) return; // typing within the same site -> nothing to swap
        // Leaving favourites mode when switching site keeps state clean.
        if (m_favoritesActive) exitFavoritesView();
        m_uiSite = s;
        updateQueryPlaceholder();
        applyHistoryToUi();
        if (m_favoritesBtn)
            m_favoritesBtn->setVisible(isFavouritableSite(s));
        // On the empty landing, switching to a favouritable site swaps in its home feed.
        maybeBuildHomeFeed();
    });
    // Enter in the site box jumps to the query so you can just type and search.
    connect(siteBar->lineEdit(), &QLineEdit::returnPressed, this, [this]() { queryBar->setFocus(); });
    updateQueryPlaceholder(); // initial prompt ("Search YouTube")

    connect(backBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex > 0)       navigateTo(m_navIndex - 1);
        else if (m_navIndex == 0) goHome(); // back from the first search returns to the home feed
    });

    connect(forwardBtn, &QPushButton::clicked, this, [this]() {
        if (m_navIndex < m_navHistory.size() - 1) navigateTo(m_navIndex + 1);
    });

    connect(homeBtn, &QPushButton::clicked, this, [this]() { goHome(); });

    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        // Home feed view (no nav entry): rebuild it from the favourite channels/models.
        if (m_homeBuilt && m_navIndex < 0 && m_viewMode == ViewMode::Search
            && isFavouritableSite(currentSite())) {
            loadHomeFeed();
            return;
        }
        if (m_navIndex < 0 || m_navIndex >= m_navHistory.size()) return;
        for (const SearchResult& r : m_allResults)
            if (!r.thumbnail.isEmpty()) QPixmapCache::remove(r.thumbnail);
        // Clear the cache so navigateTo re-fetches instead of restoring.
        m_navHistory[m_navIndex].results.clear();
        m_navHistory[m_navIndex].channelInfo = {};
        navigateTo(m_navIndex);
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

    // When a channel/model is starred or unstarred, refresh the favourites view (if open)
    // or the landing home feed — but only when the change is for the site on screen, so a
    // YouTube toggle doesn't disturb a PornHub view and vice versa. Each store is wired to
    // its own site.
    auto wireFavStore = [this](SgFavorites* store, SgSearch::Site site) {
        connect(store, &SgFavorites::changed, this, [this, site](const QString&, bool) {
            if (currentSite() != site) return; // change is for a site not currently shown
            // This site's home feed is now stale -> force a rebuild next time it's shown
            // (e.g. starring from a results page, then hitting Home/back).
            if (m_homeFeedSite == site) { m_homeBuilt = false; m_homeCache.clear(); }
            if (m_favoritesActive) { enterFavoritesView(); return; }
            // Curating favourites on the home/landing view -> reflect the new set.
            // Bounded: one light rebuild per star toggle.
            if (!m_homeLoading && m_navIndex < 0 && m_viewMode == ViewMode::Search
                && m_currentQuery.isEmpty())
                loadHomeFeed();
        });
    };
    wireFavStore(SgFavorites::instance(),   SgSearch::Site::YouTube);
    wireFavStore(SgFavorites::phInstance(), SgSearch::Site::PornHub);
    wireFavStore(SgFavorites::cbInstance(), SgSearch::Site::Chaturbate);

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

    // First open on YouTube with nothing loaded -> build the favourites home feed once
    // per session. startup warming (warmHomeFeed) usually beats this to the punch.
    maybeBuildHomeFeed();
}

void Search::warmHomeFeed() {
    // Called from startup once the update flow has settled, so the landing feed is
    // already populated by the time the user clicks over to this tab. Shares the
    // showEvent guard, so it harmlessly does nothing if the feed is already up.
    maybeBuildHomeFeed();
}

void Search::maybeBuildHomeFeed() {
    // Build only when this tab is sitting on its landing view (navIndex -1, not on a
    // search/channel page) on a favouritable site, and that site's feed isn't already up.
    // This also fires when switching between YouTube and PornHub on the landing, swapping
    // the home feed (and showing the "favourite some …" prompt when there are none).
    const SgSearch::Site site = currentSite();
    if (m_homeLoading || !isFavouritableSite(site)) return;
    if (m_navIndex >= 0) return;                          // on a search/channel, not the landing
    if (m_homeBuilt && m_homeFeedSite == site) return;    // this site's feed already shown
    loadHomeFeed();
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

    // Layout from right to left: [favorites] [sort] [search / expanding bar]
    int nextRight = rightEdge;

    // The favourites chip is on favouritable sites; leave space for it only there.
    if (m_favoritesBtn && isFavouritableSite(currentSite())) {
        m_favoritesBtn->move(nextRight - m_favoritesBtn->width(), kPillTopMargin);
        m_favoritesBtn->raise();
        nextRight -= m_favoritesBtn->width() + gap;
    }

    m_resultSortBtn->move(nextRight - m_resultSortBtn->width(), kPillTopMargin);
    m_resultSortBtn->raise();
    nextRight -= m_resultSortBtn->width() + gap;

    m_resultSearchBtn->move(nextRight - m_resultSearchBtn->width(), kPillTopMargin);
    m_resultSearchBtn->raise();
    m_resultSearchBar->move(nextRight - m_resultSearchBar->width(),
        kPillTopMargin + (m_resultSearchBtn->height() - m_resultSearchBar->height()) / 2);
    m_resultSearchBar->raise();
}

void Search::updateFilterPillVisibility() {
    const bool atTop = (resultsArea->verticalScrollBar()->value() <= 0);
    // Zone is relative to resultsArea (the chips' parent); tall enough to cover the
    // chips (taller than the pill) so hovering them doesn't fall outside and hide them.
    const int stripH = qMax(m_filterPill->height(), m_resultSortBtn->height());
    const QRect zone(0, 0, resultsArea->width(), stripH + 2 * kPillTopMargin);
    const bool hovered = zone.contains(resultsArea->mapFromGlobal(QCursor::pos()));
    const bool show = atTop || hovered;
    // The Videos/Shorts pill is YouTube-only; the favourites chip is on favouritable sites.
    const bool isYouTube = (currentSite() == SgSearch::Site::YouTube);
    m_filterPill->setVisible(show && isYouTube);
    m_resultSortBtn->setVisible(show);
    if (m_favoritesBtn) {
        const bool favShow = show && isFavouritableSite(currentSite());
        if (m_favoritesBtn->isVisible() != favShow) {
            m_favoritesBtn->setVisible(favShow);
            positionTopControls(); // reflow the chip row when the star appears/disappears
        }
    }

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

void Search::tintFavoritesIcon() {
    if (!m_favoritesBtn) return;
    const QSize sz(18, 18);
    const QString path = m_favoritesActive
        ? QStringLiteral(":/Assets/icons/star.svg")
        : QStringLiteral(":/Assets/icons/star-outline.svg");
    QPixmap pm = QIcon(path).pixmap(sz);
    if (pm.isNull()) return;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    // Active: accent colour so the button reads as "on"; inactive: same dim tint as the other chips.
    const QColor color = m_favoritesActive
        ? palette().color(QPalette::Highlight)
        : palette().color(QPalette::WindowText);
    p.fillRect(pm.rect(), color);
    p.end();
    m_favoritesBtn->setIcon(QIcon(pm));
    m_favoritesBtn->setIconSize(sz);
}

void Search::enterFavoritesView() {
    m_favoritesActive = true;

    // Clear the grid and fill it with favourites — m_allResults is left untouched
    // so background search batches can still accumulate while the overlay is open.
    QLayoutItem* item;
    while ((item = resultsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    SgFavorites* store = favStore();
    const auto favs = store ? store->favorites() : QList<SgFavorites::FavoriteChannel>{};
    // Chaturbate "channels" are live rooms, so their favourite cards are playable rooms
    // (isChannel=false) — clicking opens the room. YouTube/PornHub favourites are channel
    // cards that open the channel/model page.
    const bool channelCards = currentSite() != SgSearch::Site::Chaturbate;
    for (const auto& fav : favs) {
        SearchResult r;
        r.isChannel  = channelCards;
        r.url        = fav.url;
        r.channelUrl = fav.url;
        r.channel    = fav.name;
        r.title      = fav.name;
        if (!fav.cachedThumbPath.isEmpty())
            r.thumbnail = QUrl::fromLocalFile(fav.cachedThumbPath).toString();
        else if (!fav.thumbnailUrl.isEmpty()) {
            r.thumbnail = fav.thumbnailUrl;
            if (fav.url.contains("chaturbate.com", Qt::CaseInsensitive))
                r.thumbnailReferer = QStringLiteral("https://chaturbate.com/"); // CDN needs it
        }
        addCard(r);
    }

    applyCardWidth();

    if (favs.isEmpty())
        setStatus(currentSite() == SgSearch::Site::PornHub
            ? "No favourite models yet. Star a model to add it."
            : "No favourite channels yet. Star a channel to add it.", false);
    else
        setStatus("", false);

    tintFavoritesIcon();
}

void Search::exitFavoritesView() {
    m_favoritesActive = false;

    // m_allResults was never touched while favourites was open, so just rebuild
    // the grid from it directly — no save/restore needed.
    rebuildCards();

    if (m_allResults.isEmpty())
        setStatus(emptyStateText(), false);
    else
        setStatus("", false);

    tintFavoritesIcon();
}

bool Search::isFavouritableSite(SgSearch::Site s) {
    return s == SgSearch::Site::YouTube || s == SgSearch::Site::PornHub
        || s == SgSearch::Site::Chaturbate;
}

SgFavorites* Search::favStore() const {
    switch (currentSite()) {
    case SgSearch::Site::YouTube:    return SgFavorites::instance();
    case SgSearch::Site::PornHub:    return SgFavorites::phInstance();
    case SgSearch::Site::Chaturbate: return SgFavorites::cbInstance();
    default:                         return nullptr;
    }
}

QString Search::emptyStateText() const {
    // On a favouritable site the landing view is the favourites home feed, so when there
    // are no favourites yet, nudge the user to add some instead of prompting a search.
    if (SgFavorites* store = favStore(); store && store->favorites().isEmpty()) {
        const SgSearch::Site s = currentSite();
        return (s == SgSearch::Site::PornHub || s == SgSearch::Site::Chaturbate)
            ? QStringLiteral("Favorite some models to fill your homepage.")
            : QStringLiteral("Favorite some channels to fill your homepage.");
    }
    return "Search " + siteName() + " to see results.";
}

QStringList Search::homeChannels() const {
    SgFavorites* store = favStore();
    if (!store) return {};
    QStringList all;
    for (const auto& f : store->favorites()) all << f.url;
    if (all.isEmpty()) return {};

    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    const QString suffix = homeSiteSuffix(currentSite());
    // Per-site result limit (1..20). Falls back to the old global key so existing users
    // keep their chosen amount the first time after upgrading.
    const int legacy = cfg.value("Search/HomeChannelAmount", 5).toInt();
    const int amount = qBound(1, cfg.value("Search/HomeAmount" + suffix, legacy).toInt(), 20);
    const QString key = "Search/HomeChannels" + suffix;

    // The Settings picker stores the user's chosen channels in priority order. Use them
    // (still-favourited), capped at `amount`. If they haven't picked any, fall back to the
    // first `amount` favourites — so the home feed works without ever opening Settings.
    QStringList out;
    for (const QString& u : cfg.value(key).toStringList())
        if (all.contains(u) && out.size() < amount) out << u;
    if (out.isEmpty())
        for (const QString& u : all) { if (out.size() >= amount) break; out << u; }
    return out;
}

int Search::homeVideosPerChannel() const {
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    // Per-site, falling back to the old global key so upgrades keep the prior value.
    // (Chaturbate never reaches here — its home feed is favourited rooms, not a per-channel pull.)
    const int legacy = cfg.value("Search/HomeVideosPerChannel", kHomePerChannel).toInt();
    const QString key = "Search/HomeVideosPerChannel" + homeSiteSuffix(currentSite());
    return qBound(1, cfg.value(key, legacy).toInt(), 20);
}

void Search::loadHomeFeed() {
    const SgSearch::Site site = currentSite();
    if (!m_search || !isFavouritableSite(site)) return;

    // The home feed IS the landing view: clear the grid and sit at navIndex -1 so the
    // page is empty before results arrive (and stays empty, with the prompt, when there
    // are no favourites — that's the empty-page-on-switch behaviour).
    if (m_favoritesActive) { m_favoritesActive = false; tintFavoritesIcon(); }
    setViewMode(ViewMode::Search);
    clearResults();
    m_currentQuery.clear();
    m_currentSite  = site;
    m_homeFeedSite = site;               // remember the site so a mid-build switch aborts
    m_homePerChannel = homeVideosPerChannel(); // videos per channel for this build (Settings)
    m_navIndex     = -1;                 // home/landing
    m_shownCount  = 0;
    m_loadingMore = false;
    m_endReached  = true;                // the home feed doesn't scroll-paginate
    refreshBtn->setEnabled(true);        // refresh rebuilds the home feed
    updateNavButtons();

    const QStringList channels = homeChannels();
    if (channels.isEmpty()) {            // no favourites yet — empty page + the prompt
        m_homeBuilt = true;             // this site's (empty) landing is "built"
        m_homeCache.clear();
        rebuildCards();                  // grid is now empty
        setStatus(emptyStateText(), false);
        return;
    }

    if (site == SgSearch::Site::Chaturbate) {
        // Chaturbate models are live rooms, not video lists — there's nothing to fetch
        // per channel. The home feed is simply your favourited models (in picker order,
        // already capped by homeChannels()), each card opening its live room.
        SgFavorites* store = favStore();
        QHash<QString, SgFavorites::FavoriteChannel> byUrl;
        if (store) for (const auto& f : store->favorites()) byUrl.insert(f.url, f);
        for (const QString& url : channels) {
            const auto it = byUrl.constFind(url);
            if (it == byUrl.constEnd()) continue;
            const SgFavorites::FavoriteChannel& f = it.value();
            SearchResult r;
            r.url        = f.url;          // the live room — playable on click
            r.channelUrl = f.url;          // routes the favourite star to the CB store
            r.channel    = f.name;
            r.title      = f.name;
            if (!f.cachedThumbPath.isEmpty())
                r.thumbnail = QUrl::fromLocalFile(f.cachedThumbPath).toString();
            else if (!f.thumbnailUrl.isEmpty()) {
                r.thumbnail = f.thumbnailUrl;
                r.thumbnailReferer = QStringLiteral("https://chaturbate.com/"); // CDN needs it
            }
            if (!r.url.isEmpty()) m_seenUrls.insert(r.url);
            m_allResults.append(r);
        }
        pumpHomeFeed(); // empty queue -> runs the finalise tail (sort/cache/cards/status)
        return;
    }

    m_homeQueue   = channels;
    m_homeLoading = true;
    setStatus("Loading your channels.", true);
    pumpHomeFeed();
}

void Search::pumpHomeFeed() {
    // More channels queued -> fetch the next one. Drained -> finalise the feed.
    if (!m_homeQueue.isEmpty()) {
        m_search->fetchChannelVideos(m_homeQueue.takeFirst(), m_homePerChannel);
        return;
    }
    m_homeLoading = false;
    m_homeBuilt   = true;
    m_shownCount  = m_allResults.size();
    applySort();                          // default Newest mixes channels by recency
    m_homeCache   = m_allResults;         // cache the built feed so back/Home restore it
    rebuildCards();
    setStatus(m_allResults.isEmpty() ? emptyStateText() : QString(), false);
}

void Search::goHome() {
    // Snapshot the page we're leaving so forward can restore it, then drop to the
    // landing (navIndex -1) and show the current site's home feed.
    if (m_navIndex >= 0 && m_navIndex < m_navHistory.size())
        m_navHistory[m_navIndex].results = m_allResults;
    if (m_favoritesActive) { m_favoritesActive = false; tintFavoritesIcon(); }

    const SgSearch::Site site = currentSite();

    // Sites without favourites (Chaturbate) have no home feed — just clear to the prompt.
    // Otherwise restore the cached feed instantly when it's for this site; else rebuild.
    if (isFavouritableSite(site) && m_homeBuilt && m_homeFeedSite == site) {
        m_homeLoading = false; m_homeQueue.clear();
        m_navIndex = -1;
        setViewMode(ViewMode::Search);
        clearResults();
        m_currentQuery.clear();
        m_currentSite = site;
        m_allResults  = m_homeCache;
        m_shownCount  = m_homeCache.size();
        m_endReached  = true;
        refreshBtn->setEnabled(true);
        for (const SearchResult& r : m_allResults)
            if (!r.url.isEmpty()) m_seenUrls.insert(r.url);
        rebuildCards();
        setStatus(m_allResults.isEmpty() ? emptyStateText() : QString(), false);
        updateNavButtons();
    } else if (isFavouritableSite(site)) {
        loadHomeFeed(); // first time on this site's home, or a different site — build it
    } else {
        m_navIndex = -1;
        setViewMode(ViewMode::Search);
        clearResults();
        m_currentQuery.clear();
        setStatus(emptyStateText(), false);
        updateNavButtons();
    }
}

void Search::handleHomeBatch(const SearchResult& /*info*/, const QList<SearchResult>& videos) {
    // A site switch (or any other change) during the build abandons it.
    if (currentSite() != m_homeFeedSite) {
        m_homeLoading = false; m_homeQueue.clear(); return;
    }

    int added = 0;
    for (const SearchResult& v : videos) {
        if (added >= m_homePerChannel) break;
        if (v.isChannel) continue;
        if (!v.url.isEmpty()) {
            if (m_seenUrls.contains(v.url)) continue;
            m_seenUrls.insert(v.url);
        }
        SearchResult r = v;
        r.seq = m_seqCounter++;
        m_allResults.append(r);
        ++added;
    }

    if (!m_homeQueue.isEmpty()) rebuildCards(); // show progress as each channel lands
    pumpHomeFeed();
}

void Search::showResultSortMenu() {
    m_resultSortMenu->popup(m_resultSortBtn->mapToGlobal(QPoint(0, m_resultSortBtn->height())));
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

void Search::pushNavEntry(const NavEntry& entry) {
    // Snapshot the current entry's results before cutting forward history.
    if (m_navIndex >= 0 && m_navIndex < m_navHistory.size())
        m_navHistory[m_navIndex].results = m_allResults;
    if (m_navIndex < m_navHistory.size() - 1)
        m_navHistory = m_navHistory.mid(0, m_navIndex + 1);
    m_navHistory.append(entry);
    m_navIndex = m_navHistory.size() - 1;
    updateNavButtons();
}

void Search::navigateTo(int index) {
    if (index < 0 || index >= m_navHistory.size()) return;
    m_homeLoading = false; m_homeQueue.clear(); // navigating supersedes a home build

    // Snapshot the current entry's results before leaving it (only when actually
    // changing entries; refresh calls navigateTo with the same index and must not
    // overwrite the cache it just cleared).
    if (m_navIndex >= 0 && m_navIndex < m_navHistory.size() && m_navIndex != index)
        m_navHistory[m_navIndex].results = m_allResults;

    m_navIndex = index;
    const NavEntry& e = m_navHistory[index];
    queryBar->setCurrentText(e.label);

    if (!e.results.isEmpty()) {
        // Restore from cache: no network fetch needed.
        if (e.kind == NavEntry::Channel) {
            if (m_favoritesActive) exitFavoritesView();
            setViewMode(ViewMode::Channel);
            m_currentChannelUrl = e.target;
            if (!e.channelInfo.title.isEmpty())
                updateChannelHeader(e.channelInfo);
            else {
                m_channelName->setText(e.label);
                m_channelSubs->clear();
                loadAvatar(QString());
            }
        } else {
            // Exit favourites view cleanly before restoring a search page.
            if (m_favoritesActive) {
                m_favoritesActive = false;
                tintFavoritesIcon();
            }
            setViewMode(ViewMode::Search);
            siteBar->blockSignals(true);
            siteBar->setCurrentText(e.site == SgSearch::Site::PornHub ? "PornHub" : "YouTube");
            siteBar->blockSignals(false);
            m_currentQuery = e.target;
            m_currentSite  = e.site;
        }
        clearResults();
        m_allResults  = e.results;
        m_shownCount  = e.results.size();
        m_endReached  = true; // don't auto-load-more on a restored page
        refreshBtn->setEnabled(true);
        // Rebuild the seen-URL set from the restored results for dedup consistency.
        for (const SearchResult& r : m_allResults)
            if (!r.url.isEmpty()) m_seenUrls.insert(r.url);
        rebuildCards();
        setStatus("", false);
    } else {
        // No cache yet — fetch normally (first visit or after a forced refresh).
        if (e.kind == NavEntry::Channel) {
            if (m_favoritesActive) exitFavoritesView();
            openChannelUrl(e.target, e.label);
        } else {
            // Restore the site this query ran against so startSearch routes correctly.
            siteBar->blockSignals(true);
            siteBar->setCurrentText(e.site == SgSearch::Site::PornHub ? "PornHub" : "YouTube");
            siteBar->blockSignals(false);
            m_currentQuery = e.target;
            m_currentSite  = e.site;
            startSearch(e.target);
        }
    }
    updateNavButtons();
}

void Search::updateNavButtons() {
    // Back is enabled on any nav entry (index >= 0): from the first one it returns to
    // the home feed (navIndex -1). Forward walks toward the newest entry; from the home
    // feed it re-enters the first entry.
    backBtn->setEnabled(m_navIndex >= 0);
    forwardBtn->setEnabled(m_navIndex < m_navHistory.size() - 1);
}

void Search::addToHistory(const QString& query) {
    QStringList& h = m_historyFor[static_cast<int>(currentSite())]; // the searched site's history
    h.removeAll(query);
    h.prepend(query);
    if (h.size() > 50) h.removeLast();
    m_historyModel->setStringList(h);

    // Mirror into the combo's item list incrementally (a full rebuild would
    // disturb the edit text mid-search).
    queryBar->blockSignals(true);
    const int dup = queryBar->findText(query, Qt::MatchFixedString);
    if (dup >= 0) queryBar->removeItem(dup);
    queryBar->insertItem(0, query);
    while (queryBar->count() > 50) queryBar->removeItem(queryBar->count() - 1);
    queryBar->setCurrentIndex(0); // the bar keeps showing the running query
    queryBar->blockSignals(false);

    saveHistory(currentSite());
}

// History is per-site, each in its own plain-text file inside the Config folder
// (one query per line, most recent first).
QString Search::historyFilePath(SgSearch::Site site) {
    const QString base = SgPaths::configDir();
    switch (site) {
    case SgSearch::Site::PornHub:    return base + "/search_history_ph.txt";
    case SgSearch::Site::Chaturbate: return base + "/search_history_cb.txt";
    default:                         return base + "/search_history.txt";
    }
}

void Search::loadHistory() {
    for (int i = 0; i < 3; ++i) {
        m_historyFor[i].clear();
        QFile f(historyFilePath(static_cast<SgSearch::Site>(i)));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue; // none yet
        QTextStream in(&f);
        while (!in.atEnd() && m_historyFor[i].size() < 50) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty()) m_historyFor[i].append(line);
        }
    }
    applyHistoryToUi();
}

void Search::applyHistoryToUi() {
    const QStringList& h = m_historyFor[static_cast<int>(currentSite())];
    const QString pending = queryBar->currentText(); // keep any in-progress query text
    m_historyModel->setStringList(h);
    queryBar->blockSignals(true);
    queryBar->clear();
    queryBar->addItems(h);
    queryBar->setEditText(pending);
    queryBar->blockSignals(false);
}

void Search::saveHistory(SgSearch::Site site) {
    QFile f(historyFilePath(site));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream out(&f);
    for (const QString& q : m_historyFor[static_cast<int>(site)]) out << q << '\n';
}

void Search::clearSearchHistory() {
    for (int i = 0; i < 3; ++i) { // clear every site's history + file
        m_historyFor[i].clear();
        QFile::remove(historyFilePath(static_cast<SgSearch::Site>(i)));
    }
    m_historyModel->setStringList({});
    queryBar->blockSignals(true);
    queryBar->clear(); // items and edit text both go
    queryBar->blockSignals(false);
}

// ---------------------------------------------------------------------------
// Search execution
// ---------------------------------------------------------------------------

void Search::performSearch() {
    // One Enter fires returnPressed AND textActivated; the twin runs right after this
    // call returns (after the duplicate-site modal closes, if shown). Collapse the pair
    // with a timestamp re-stamped at the END — robust across the modal's nested event
    // loop, unlike the old singleShot flag whose reset fired mid-prompt.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastSearchFireMs && now - m_lastSearchFireMs < 400) return;
    m_lastSearchFireMs = now;

    const QString query = queryBar->currentText().trimmed();

    if (query.isEmpty()) {
        setStatus("Enter something to search for.", false);
        return;
    }
    if (!m_search) {
        setStatus("Search backend unavailable.", false);
        return;
    }

    // Another search tab already on this site? Warn once before adding load (bot risk).
    if (!confirmSharedSiteSearch()) {
        m_lastSearchFireMs = QDateTime::currentMSecsSinceEpoch(); // swallow the twin after the modal
        return;
    }

    pushNavEntry({ NavEntry::Query, query, query, currentSite() });
    addToHistory(query);
    startSearch(query);
    m_lastSearchFireMs = QDateTime::currentMSecsSinceEpoch(); // re-stamp after the (possibly long) modal
}

void Search::startSearch(const QString& query) {
    m_homeLoading = false; m_homeQueue.clear(); // a real search supersedes a home build
    // Exit favourites view before a real search so the grid is clean.
    if (m_favoritesActive) {
        m_favoritesActive = false;
        tintFavoritesIcon();
    }
    setViewMode(ViewMode::Search);
    clearResults();
    m_currentQuery = query;
    m_currentSite  = currentSite(); // remember the site for paging (loadMore)
    m_shownCount   = 0;
    m_loadingMore  = false;
    m_endReached   = false;
    refreshBtn->setEnabled(true);

    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
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
    if (m_favoritesActive) return;
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

void Search::playRandomResult() {
    if (m_allResults.isEmpty()) return;
    // Collect the indices that currently pass the filter, then pick one that
    // isn't already playing.
    QList<int> shown;
    for (int i = 0; i < m_allResults.size(); ++i)
        if (shows(m_allResults[i])) shown.append(i);
    if (shown.isEmpty()) return;
    int pick = shown.at(QRandomGenerator::global()->bounded(shown.size()));
    if (shown.size() > 1 && pick == m_playingIndex) {
        const int pos = shown.indexOf(pick);
        pick = shown.at((pos + 1) % shown.size());
    }
    playResultAt(pick);
}

QString Search::playbackContextKey() const {
    switch (m_currentSite) {
    case SgSearch::Site::PornHub:    return QStringLiteral("search.pornhub");
    case SgSearch::Site::Chaturbate: return QStringLiteral("search.chaturbate");
    default:                         return QStringLiteral("search.youtube");
    }
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
    if (m_homeLoading) { handleHomeBatch(channelInfo, videos); return; } // home feed build
    if (m_viewMode != ViewMode::Channel) return; // late reply after navigating away
    if (m_shownCount == 0) {
        updateChannelHeader(channelInfo); // first page fills the header
        // Cache the channel header info so back/forward can restore it without re-fetching.
        if (m_navIndex >= 0 && m_navIndex < m_navHistory.size())
            m_navHistory[m_navIndex].channelInfo = channelInfo;
    }
    ingestResults(videos);
}

void Search::ingestResults(const QList<SearchResult>& results) {
    m_loadingMore = false;

    if (results.size() < m_lastRequested || results.size() <= m_shownCount)
        m_endReached = true;

    if (m_shownCount == 0 && results.isEmpty()) {
        if (!m_favoritesActive) setStatus("No results.", false);
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
    if (m_shownCount >= kMaxResults) m_endReached = true;

    // Always keep the nav cache consistent with the real result list.
    if (m_navIndex >= 0 && m_navIndex < m_navHistory.size())
        m_navHistory[m_navIndex].results = m_allResults;

    // Favourites overlay is active — data accumulated, but grid is frozen.
    // The grid rebuilds correctly when the user exits favourites.
    if (m_favoritesActive) return;

    if (m_sortMode != SortMode::Relevance) {
        // A non-default sort means the new arrivals have to merge into the order, so
        // re-sort and rebuild the grid wholesale.
        applySort();
        rebuildCards();
    } else {
        // Relevance: the new arrivals belong at the end — just append their cards.
        for (int i = before; i < m_allResults.size(); ++i)
            if (shows(m_allResults[i])) addCard(m_allResults[i]);
    }

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
    // A channel fetch failed mid home-build — skip it and continue with the rest.
    if (m_homeLoading) { pumpHomeFeed(); return; }
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
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
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
    if (m_favoritesActive) return;
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
    if (m_favoritesActive) exitFavoritesView();
    pushNavEntry({ NavEntry::Channel, channelUrl, name });
    openChannelUrl(channelUrl, name);
}

void Search::openChannelUrl(const QString& channelUrl, const QString& label) {
    if (!m_search) return;
    m_homeLoading = false; m_homeQueue.clear(); // opening a channel supersedes a home build
    setViewMode(ViewMode::Channel);
    queryBar->setCurrentText(label); // address-bar shows where you are
    clearResults();
    m_currentChannelUrl = channelUrl;
    m_shownCount  = 0;
    m_loadingMore = false;
    m_endReached  = false;
    refreshBtn->setEnabled(true);

    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
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
    if (s.contains("chaturbate") || s == "cb") return SgSearch::Site::Chaturbate;
    if (s.contains("porn") || s == "ph")       return SgSearch::Site::PornHub;
    return SgSearch::Site::YouTube;
}

QString Search::siteName() const {
    switch (currentSite()) {
    case SgSearch::Site::PornHub:    return QStringLiteral("PornHub");
    case SgSearch::Site::Chaturbate: return QStringLiteral("Chaturbate");
    default:                         return QStringLiteral("YouTube");
    }
}

Search::~Search() {
    s_instances.removeOne(this);
}

bool Search::anotherTabOnSite(SgSearch::Site site) const {
    for (Search* s : s_instances)
        if (s != this && s->currentSite() == site) return true;
    return false;
}

bool Search::confirmSharedSiteSearch() {
    if (m_dupeSiteAcknowledged) return true; // already warned + continued in this tab
    if (m_dupePromptOpen) return false;      // a prompt is already up — don't stack a twin
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    if (!cfg.value("Search/WarnDuplicateSite", true).toBool()) return true; // opted out app-wide
    if (!anotherTabOnSite(currentSite())) return true;                      // no conflict

    m_dupePromptOpen = true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Multiple searches, same site");
    box.setText("Another search tab is already set to " + siteName() + ".");
    box.setInformativeText(
        "Running more than one search against the same site at once raises the chance "
        "of being flagged as a bot. Seagull recommends one search tab per site.");
    QPushButton* cont = box.addButton("Continue Search", QMessageBox::AcceptRole);
    box.addButton("Cancel", QMessageBox::RejectRole);
    box.setDefaultButton(cont);
    QCheckBox* dontShow = new QCheckBox("Don't show this again", &box);
    box.setCheckBox(dontShow);
    box.exec();
    m_dupePromptOpen = false;

    if (dontShow->isChecked())
        cfg.setValue("Search/WarnDuplicateSite", false); // never warn again, anywhere
    const bool proceed = (box.clickedButton() == cont);
    if (proceed) m_dupeSiteAcknowledged = true; // don't nag this tab again this session
    return proceed;
}

void Search::updateQueryPlaceholder() {
    if (queryBar->lineEdit())
        queryBar->lineEdit()->setPlaceholderText("Search " + siteName());
    // Reflect the site in the idle prompt at the bottom (only while nothing's loaded).
    if (m_viewMode == ViewMode::Search && m_allResults.isEmpty())
        setStatus(emptyStateText(), false);
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
        tintFavoritesIcon();
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
