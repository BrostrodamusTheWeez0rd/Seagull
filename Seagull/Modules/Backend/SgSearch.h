#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QSet>

class QJsonObject;
class QJsonValue;
class QNetworkAccessManager;
class QNetworkReply;

// One search hit, site-agnostic. Durations are in seconds (-1 unknown), view
// counts -1 when unknown. url is the page URL the player/queue can consume.
struct SearchResult {
    QString title;
    QString url;
    QString channel;
    QString thumbnail;
    qint64  duration  = -1;
    qint64  viewCount = -1;
    bool    isShort   = false;
};

// Backend search worker — a peer to SgYtDlp, dedicated to discovery rather than
// download/stream resolution. Wraps the yt-dlp process for now; the SearchSite
// enum is the seam for adding more sites later (each gets its own arg-building
// and result-parsing branch). Runs one query at a time, async via QProcess.
//
// Shorts are the exception: yt-dlp's search extractor can't parse YouTube's
// shorts result renderers (shortsLockupViewModel) at all, so shortsOnly
// searches go straight to YouTube's internal search API over QNetworkAccess-
// Manager instead, paging via continuation tokens. Pages accumulate per query
// so the UI's grow-the-limit "load more" calls only fetch what's missing.
class SgSearch : public QObject {
    Q_OBJECT
public:
    enum class Site { YouTube };

    explicit SgSearch(QObject* parent = nullptr);
    ~SgSearch();

    void search(Site site, const QString& query, int limit = 20, bool shortsOnly = false);
    void cancel();

signals:
    void resultsReady(const QList<SearchResult>& results);
    void failed(const QString& message);
    void logMessage(const QString& message);

private slots:
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QList<SearchResult> parseYoutube(const QJsonObject& root) const;

    void startShortsSearch(const QString& query, int limit);
    void fetchShortsPage();
    void handleShortsReply();
    static void collectObjects(const QJsonValue& v, const QString& key,
                               QList<QJsonObject>& out);

    QProcess*  m_process;
    QByteArray m_buffer;
    Site       m_site = Site::YouTube;
    bool       m_cancelled = false; // suppresses the finished handler for a killed query

    // Shorts search state (YouTube internal API path). Results accumulate per
    // query across continuation pages; a new query resets the lot.
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply*         m_shortsReply = nullptr;
    QString                m_shortsQuery;
    QList<SearchResult>    m_shortsResults;
    QSet<QString>          m_shortsSeenIds;       // de-dupe across pages
    QString                m_shortsContinuation;  // next-page token, empty = none yet/exhausted
    bool                   m_shortsExhausted = false;
    int                    m_shortsLimit = 20;
};
