#ifndef SEARCH_H
#define SEARCH_H

#include <QWidget>
#include <QUrl>
#include <QList>

class QLineEdit;
class QPushButton;
class QScrollArea;
class QLabel;
class QNetworkAccessManager;
class QMovie;
class FlowLayout;
class SgSearch;
struct SearchResult;

// The Search tab. Browser-style chrome: a top "site" bar (which site to search)
// flanked by back / forward / refresh on the left and a Go button on the right,
// with the actual search-query bar underneath; results render as VideoCards in a
// reflowing grid below. For now only YouTube is supported, and the nav buttons
// are placed but not wired (back/forward will later page through cached result
// sets; refresh re-runs the current search).
class Search : public QWidget {
    Q_OBJECT
public:
    // The orchestrator passes the shared search worker in, like the other tabs.
    explicit Search(SgSearch* searchWorker, QWidget* parent = nullptr);

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

private:
    bool siteIsYoutube() const;  // site bar names YouTube in any form
    void clearResults();         // tear down the current card grid

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
    QLabel*      statusLabel;
    QLabel*      statusSpinner; // animated seagull shown while a search is in flight
    QMovie*      statusMovie;

    void setStatus(const QString& text, bool busy); // text + optional spinner

    SgSearch*               m_search;  // owned by the orchestrator
    QNetworkAccessManager*  m_nam;     // shared thumbnail fetcher for the cards
};

#endif // SEARCH_H
