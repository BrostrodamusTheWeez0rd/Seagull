#ifndef SEARCH_H
#define SEARCH_H

#include <QWidget>
#include <QUrl>
#include <QList>
#include <QStringList>
#include "../Backend/SgSearch.h"  // SearchResult (full definition needed for m_allResults)

class QLineEdit;
class QPushButton;
class QScrollArea;
class QLabel;
class QFrame;
class QNetworkAccessManager;
class QMovie;
class QCompleter;
class QStringListModel;
class QTimer;
class FlowLayout;
class VideoCard;
class SgSearch;

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
    explicit Search(SgSearch* searchWorker, QWidget* parent = nullptr);

public slots:
    void setCardWidth(int targetWidth);

signals:
    void playMediaRequested(const QUrl& rawUrl, const QUrl& cdnVideoUrl,
        const QUrl& cdnAudioUrl, const QString& title);
    void enqueueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);

private slots:
    void performSearch();
    void onResultsReady(const QList<SearchResult>& results);
    void onSearchFailed(const QString& message);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showEvent(QShowEvent*  event) override;
    void hideEvent(QHideEvent*  event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class FilterMode { All, Videos, Shorts };

    bool siteIsYoutube() const;
    void clearResults();
    void addCard(const SearchResult& result);
    void rebuildCards();
    void loadMore();
    int  fillCardWidth() const;
    void applyCardWidth();
    void setStatus(const QString& text, bool busy);

    void startSearch(const QString& query);
    void pushNavEntry(const QString& query);
    void updateNavButtons();
    void addToHistory(const QString& query);
    void setFilterMode(FilterMode mode);
    bool passesFilter(const SearchResult& r) const;
    void positionFilterPill();
    void updateFilterPillVisibility();

    static constexpr int kPillTopMargin = 8;

    // Chrome row
    QPushButton* backBtn;
    QPushButton* forwardBtn;
    QPushButton* refreshBtn;
    QLineEdit*   siteBar;
    QPushButton* goBtn;
    QLineEdit*   queryBar;

    // Floating filter pill (Videos / Shorts) — direct child of this, not in layout
    QFrame*      m_filterPill;
    QPushButton* m_filterVideosBtn;
    QPushButton* m_filterShortsBtn;
    QTimer*      pillHoverTimer;

    QScrollArea* resultsArea;
    QWidget*     resultsHost;
    FlowLayout*  resultsFlow;
    QFrame*      statusPill;
    QLabel*      statusLabel;
    QLabel*      statusSpinner;
    QMovie*      statusMovie;

    SgSearch*              m_search;
    QNetworkAccessManager* m_nam;
    QCompleter*            m_historyCompleter;
    QStringListModel*      m_historyModel;

    QList<SearchResult> m_allResults;
    FilterMode   m_filterMode  = FilterMode::All;

    QStringList  m_navHistory;
    int          m_navIndex    = -1;

    QStringList  m_searchHistory;

    QString m_currentQuery;
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
