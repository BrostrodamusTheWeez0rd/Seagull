#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QByteArray>
#include <QList>

class QJsonObject;

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
class SgSearch : public QObject {
    Q_OBJECT
public:
    enum class Site { YouTube };

    explicit SgSearch(QObject* parent = nullptr);
    ~SgSearch();

    void search(Site site, const QString& query, int limit = 20);
    void cancel();

signals:
    void resultsReady(const QList<SearchResult>& results);
    void failed(const QString& message);
    void logMessage(const QString& message);

private slots:
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QList<SearchResult> parseYoutube(const QJsonObject& root) const;

    QProcess*  m_process;
    QByteArray m_buffer;
    Site       m_site = Site::YouTube;
    bool       m_cancelled = false; // suppresses the finished handler for a killed query
};
