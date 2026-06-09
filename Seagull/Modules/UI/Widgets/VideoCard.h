#ifndef VIDEOCARD_H
#define VIDEOCARD_H

#include <QWidget>
#include <QUrl>
#include "../../Backend/SgSearch.h" // SearchResult

class QLabel;
class QNetworkAccessManager;

// A reusable result tile: thumbnail + title + "channel | length | views", styled
// by the theme. Clicking the card (or its Play button) asks to play the video.
// Thumbnails load asynchronously through a QNetworkAccessManager shared by the
// owner so the cards don't each spin up their own.
class VideoCard : public QWidget {
    Q_OBJECT
public:
    VideoCard(const SearchResult& result, QNetworkAccessManager* nam, QWidget* parent = nullptr);

    static QString formatDuration(qint64 seconds);
    static QString formatViewCount(qint64 views);

signals:
    void playRequested(const QUrl& url, const QString& title);
    void queueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);

protected:
    void mousePressEvent(QMouseEvent* event) override; // click anywhere = play

private:
    void loadThumbnail(QNetworkAccessManager* nam);

    SearchResult m_result;
    QLabel*      m_thumb;
};

#endif // VIDEOCARD_H
