#ifndef SEARCH_H
#define SEARCH_H

#include <QWidget>
#include <QUrl>
#include <QList>

class QLineEdit;
class QPushButton;
class QScrollArea;
class QLabel;
class QFrame;
class QNetworkAccessManager;
class QMovie;
class FlowLayout;
class VideoCard;
class SgSearch;
struct SearchResult;

// The Search tab. Browser-style chrome: a top "site" bar (which site to search)
// flanked by back / forward / refresh on the left and a Go button on the right,
// with the actual search-query bar underneath; results render as fixed-size
// VideoCards in a reflowing grid below. Cards are a fixed size (Settings → Display
// "Card size"), so resizing the window just reflows them — it never re-renders
// the cards, which keeps it smooth. For now only YouTube is supported, and the nav
// buttons are placed but not wired.
class Search : public QWidget {
    Q_OBJECT
public:
    // The orchestrator passes the shared search worker in, like the other tabs.
    explicit Search(SgSearch* searchWorker, QWidget* parent = nullptr);

public slots:
    // Settings → Display "Card size" (a target/min width). Cards grow to fill the
    // row from this minimum, so this changes how many columns there are.
    void setCardWidth(int targetWidth);

signals:
    // A card was activated — mirrors Queue's signal so the orchestrator can route
    // it to the player the same way (raw URL only; CDN URLs resolve downstream).
    void playMediaRequested(const QUrl& rawUrl, const QUrl& cdnVideoUrl,
        const QUrl& cdnAudioUrl, const QString& title);

    // Card actions routed to the Queue tab's existing add/download flow.
    void enqueueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);

private slots:
    void performSearch();                                   // Go / Enter
    void onResultsReady(const QList<SearchResult>& results);
    void onSearchFailed(const QString& message);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override; // viewport resize -> refit cards

private:
    bool siteIsYoutube() const;  // site bar names YouTube in any form
    void clearResults();         // tear down the current card grid
    void addCard(const SearchResult& result); // build + wire one result card
    void loadMore();             // fetch the next batch (scroll-triggered)
    int  fillCardWidth() const;  // width that fits whole columns of >= m_targetWidth
    void applyCardWidth();       // recompute the fill width + resize every card

    // Top row: browser-style navigation + the site chooser.
    QPushButton* backBtn;
    QPushButton* forwardBtn;
    QPushButton* refreshBtn;
    QLineEdit*   siteBar;
    QPushButton* goBtn;

    QLineEdit*   queryBar;       // bottom row: the search query

    QScrollArea* resultsArea;
    QWidget*     resultsHost;
    FlowLayout*  resultsFlow;
    QFrame*      statusPill;     // rounded "pill" holding the status text + spinner
    QLabel*      statusLabel;
    QLabel*      statusSpinner;  // animated seagull shown while a search is in flight
    QMovie*      statusMovie;

    void setStatus(const QString& text, bool busy); // text + optional spinner ("" hides the pill)

    SgSearch*               m_search;  // owned by the orchestrator
    QNetworkAccessManager*  m_nam;     // shared thumbnail fetcher for the cards

    // Lazy data loading. Each batch re-requests the prefix and we append only the
    // new tail (yt-dlp search has no continuation token), capped to keep it cheap.
    QString m_currentQuery;
    int     m_batchSize = 20;     // results revealed per batch (snapshotted per search)
    int     m_shownCount = 0;     // cards currently on screen
    int     m_lastRequested = 0;  // N asked for in the most recent fetch
    bool    m_loadingMore = false;// a fetch is in flight (guards re-entry)
    bool    m_endReached = false; // YouTube ran out, or we hit the depth cap
    int     m_targetWidth = 240;  // desired/min card width (from Settings → Display)
    int     m_cardWidth = 240;    // actual width cards are grown to, to fill the row
    static constexpr int kMaxResults = 240;
};

#endif // SEARCH_H
