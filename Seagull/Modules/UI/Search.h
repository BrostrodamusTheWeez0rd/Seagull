#ifndef SEARCH_H
#define SEARCH_H

#include <QWidget>
#include <QUrl>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QElapsedTimer>
#include "../Backend/SgSearch.h"  // SearchResult (full definition needed for m_allResults)

class QLineEdit;
class QComboBox;
class QPushButton;
class QScrollArea;
class QLabel;
class QFrame;
class QMenu;
class QNetworkAccessManager;
class QMovie;
class QCompleter;
class QStringListModel;
class QTimer;
class FlowLayout;
class VideoCard;
class SgSearch;
class SgSpellCheck;
class SpellCheckLineEdit;

// The Search tab. Browser-style chrome: back / forward / refresh, a site bar,
// a Go button, and the search-query bar underneath. A bottom border on the
// chrome separates it from the results page. A small "Videos / Shorts" filter
// pill floats over the top-left of the results grid — it auto-hides when
// scrolled, reappears at the top or on hover (cursor poll like Library). Cards
// are fixed-size VideoCards in a reflowing grow-to-fill grid. A history
// completer drops down from the query bar.
//
// The Shorts pill is a source switch, not a client-side filter: shorts come
// from a different SgSearch path (YouTube's shorts search), so toggling into
// or out of Shorts re-runs the current query.
class Search : public QWidget {
    Q_OBJECT
public:
    explicit Search(SgSearch* searchWorker, SgSpellCheck* spell, QWidget* parent = nullptr);
    ~Search() override; // unregister from the shared instance list

public slots:
    void setCardWidth(int targetWidth);
    // Shorts-feed advance (wheel over the player / skip buttons): play the
    // next (+1) / previous (-1) result that passes the current filter. Walking
    // past the loaded tail pulls the next batch and continues when it lands.
    void playAdjacentResult(int delta);
    // Wipe the search history: completer entries + the persisted history file.
    // Settings' "Clear History Now" and the on-close auto-clear land here.
    void clearSearchHistory();

signals:
    void playMediaRequested(const QUrl& rawUrl, const QUrl& cdnVideoUrl,
        const QUrl& cdnAudioUrl, const QString& title);
    void enqueueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);

private slots:
    void performSearch();
    void onResultsReady(const QList<SearchResult>& results);
    void onChannelVideosReady(const SearchResult& channelInfo, const QList<SearchResult>& videos);
    void onSearchFailed(const QString& message);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showEvent(QShowEvent*  event) override;
    void hideEvent(QHideEvent*  event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override; // re-tint the magnifier/sort icons on theme change

private:
    enum class FilterMode { All, Videos, Shorts };
    // Grid ordering, mirroring the Library tab's sort. Relevance keeps YouTube's
    // ranking (the arrival order, via SearchResult::seq).
    enum class SortMode { Relevance, NameAsc, NameDesc, Newest, Oldest };
    // Search results vs. a single channel's video page. Channel pages are reached
    // and left with the same back/forward buttons (native-browser feel).
    enum class ViewMode { Search, Channel };

    // One step in the browser-style history: a search query or a channel page.
    struct NavEntry {
        enum Kind { Query, Channel };
        Kind    kind;
        QString target; // the query string, or the channel URL
        QString label;  // what to show in the query bar
        SgSearch::Site site = SgSearch::Site::YouTube; // site the query ran against (Query entries)
    };

    SgSearch::Site currentSite() const; // the site picked/typed in the dropdown
    // Multiple search tabs hammering one site looks like bot traffic. Before a
    // user-initiated search, if another open search tab is already pointed at this
    // site, warn once (with a "don't show again" opt-out, config Search/WarnDuplicateSite).
    bool anotherTabOnSite(SgSearch::Site site) const; // a sibling tab is on `site`
    bool confirmSharedSiteSearch();                   // true = proceed; false = user cancelled
    void updateQueryPlaceholder();      // set the query bar prompt to "Search <site>"
    void clearResults();
    void addCard(const SearchResult& result);
    void rebuildCards();
    void loadMore();
    int  fillCardWidth() const;
    void applyCardWidth();
    void setStatus(const QString& text, bool busy);

    void startSearch(const QString& query);
    void ingestResults(const QList<SearchResult>& results); // append + dedupe + cards + paging
    void playResultAt(int index);
    void pushNavEntry(const NavEntry& entry);
    void navigateTo(int index); // replay history entry `index` (search or channel)
    void updateNavButtons();

    // Channel pages
    void setViewMode(ViewMode mode);
    void openChannel(const QString& channelUrl, const QString& name); // pushes history, then opens
    void openChannelUrl(const QString& channelUrl, const QString& label); // opens without pushing
    void updateChannelHeader(const SearchResult& info);
    void loadAvatar(const QString& url); // channel header avatar, WebP-tolerant via ffmpeg
    void addToHistory(const QString& query);
    void loadHistory();  // read both sites' persisted history files
    void saveHistory(SgSearch::Site site); // rewrite one site's file (one query per line)
    void applyHistoryToUi(); // load the active site's history into the completer + combo
    QString siteName() const; // "YouTube" / "PornHub" for the active site
    static QString historyFilePath(SgSearch::Site site); // per-site history file
    void setFilterMode(FilterMode mode);
    bool passesFilter(const SearchResult& r) const;
    void positionFilterPill();
    void updateFilterPillVisibility();

    // Top-right magnifier + sort controls (the Library tab's chips, mirrored here
    // with the same auto-hide as the filter pill).
    void positionTopControls();     // place the magnifier + sort at the grid's top-right
    void toggleResultSearch();      // magnifier: reveal / collapse the title-filter bar
    void tintResultSearchIcon();    // recolour the magnifier glyph to the theme text
    void tintResultSortIcon();      // recolour the sort glyph to the theme text
    void showResultSortMenu();      // drop the ordering menu under the sort button
    void applySortMode(SortMode mode); // remember the choice, then re-sort + rebuild
    void applySort();               // stable-reorder m_allResults by m_sortMode; keep m_playingIndex
    bool matchesQuery(const SearchResult& r) const; // title contains the filter text
    bool shows(const SearchResult& r) const;        // passesFilter AND matchesQuery (card visibility)

    static constexpr int kPillTopMargin = 8;

    // Chrome row
    QPushButton* backBtn;
    QPushButton* forwardBtn;
    QPushButton* refreshBtn;
    QComboBox*   siteBar; // dropdown of supported search sites (YouTube / PornHub)
    QPushButton* goBtn;
    // Editable combo like the File Explorer address bar: the arrow drops the
    // full search history; typing filters it through the completer.
    QComboBox*   queryBar;

    // Floating filter pill (Videos / Shorts) — direct child of this, not in layout
    QFrame*      m_filterPill;
    QPushButton* m_filterVideosBtn;
    QPushButton* m_filterShortsBtn;
    QTimer*      pillHoverTimer;

    // Top-right chips over the grid: a magnifier that reveals a title filter, and a
    // sort button. Children of resultsArea (like the pill) so they stack above the
    // viewport; same auto-hide rules.
    QPushButton*        m_resultSearchBtn = nullptr;
    SpellCheckLineEdit* m_resultSearchBar = nullptr; // revealed on click; filters loaded results by title
    QPushButton*        m_resultSortBtn   = nullptr;
    QMenu*              m_resultSortMenu  = nullptr;
    bool                m_resultSearchOpen = false;
    QElapsedTimer       m_resultSearchOpenedClock; // grace after the click before auto-collapse
    QString             m_resultQuery;             // current title filter (lowercased on use)
    SortMode            m_sortMode  = SortMode::Newest; // loaded from config in the ctor
    int                 m_seqCounter = 0;          // running arrival index stamped onto each result

    // Channel page header (avatar + name + subscribers), shown above the grid in
    // channel view, hidden in search view. Direct child of this widget's layout.
    QFrame*      m_channelHeader;
    QLabel*      m_channelAvatar;
    QLabel*      m_channelName;
    QLabel*      m_channelSubs;

    QScrollArea* resultsArea;
    QWidget*     resultsHost;
    FlowLayout*  resultsFlow;
    QFrame*      statusPill;
    QLabel*      statusLabel;
    QLabel*      statusSpinner;
    QMovie*      statusMovie;

    // Every live Search tab, so a tab can tell if a sibling targets the same site
    // (GUI-thread only; registered in the ctor, removed in the dtor).
    static QList<Search*> s_instances;

    SgSearch*              m_search;
    SgSpellCheck*          m_spell;
    QNetworkAccessManager* m_nam;
    QCompleter*            m_historyCompleter;
    QStringListModel*      m_historyModel;

    QList<SearchResult> m_allResults;
    QSet<QString>       m_seenUrls; // dedup: YouTube search returns the same video twice
    FilterMode   m_filterMode  = FilterMode::Videos; // Videos is the launch default
    SgSearch::Site m_currentSite = SgSearch::Site::YouTube; // site of the active search (for paging)
    ViewMode     m_viewMode    = ViewMode::Search;
    QString      m_currentChannelUrl; // the channel being shown in Channel view

    // Feed position: index (into m_allResults) of the playing result, -1 none.
    // m_advancePending = a feed advance ran off the loaded tail and resumes
    // when the next batch arrives.
    int          m_playingIndex   = -1;
    bool         m_advancePending = false;

    QList<NavEntry> m_navHistory;
    int             m_navIndex = -1;

    QStringList  m_historyFor[3]; // per-site search history, indexed by int(SgSearch::Site)
    SgSearch::Site m_uiSite = SgSearch::Site::YouTube; // site the history/chrome is currently showing

    QString m_currentQuery;
    // One Enter fires returnPressed AND textActivated; the twin arrives right after
    // performSearch returns (after the duplicate-site modal closes, if shown). An
    // end-stamped timestamp collapses the pair into one search — robust across the
    // modal's nested event loop (a singleShot reset fired mid-prompt and let it through).
    qint64  m_lastSearchFireMs = 0;
    // Duplicate-site warning state: a prompt is open right now (block a stacked twin),
    // and the user already hit Continue in THIS tab — nag at most once per tab/session
    // (the "don't show again" checkbox is the permanent, app-wide opt-out via config).
    bool    m_dupePromptOpen = false;
    bool    m_dupeSiteAcknowledged = false;
    int     m_batchSize    = 20;
    int     m_shownCount   = 0;
    int     m_lastRequested = 0;
    bool    m_loadingMore  = false;
    bool    m_endReached   = false;
    int     m_targetWidth  = 240;
    int     m_cardWidth    = 240;
    static constexpr int kMaxResults = 240;
};

#endif // SEARCH_H
