#ifndef VIDEOCARD_H
#define VIDEOCARD_H

#include <QWidget>
#include <QUrl>
#include "../../Backend/SgSearch.h" // SearchResult

class QNetworkAccessManager;
class RoundedThumb;

// A reusable result tile: thumbnail + title + "channel | length | views", styled
// by the theme. Clicking the card (or its Play button) asks to play the video.
// The card is sized by the Search grid to fill the row width (grow-to-fill). Its
// thumbnail is rounded once and then *scaled* to the card size, so resizing the
// window never re-renders it — it just stretches a cached pixmap (cheap).
class VideoCard : public QWidget {
    Q_OBJECT
public:
    VideoCard(const SearchResult& result, QNetworkAccessManager* nam, int cardWidth,
        QWidget* parent = nullptr);

    void setCardWidth(int width); // grid-assigned width; thumbnail stays 16:9

    static QString formatDuration(qint64 seconds);
    static QString formatViewCount(qint64 views);

    // Card height for a given width (16:9 thumbnail + the fixed chrome below it).
    static int heightForCardWidth(int cardWidth);

signals:
    void playRequested(const QUrl& url, const QString& title);
    void queueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);

protected:
    void mousePressEvent(QMouseEvent* event) override; // click anywhere = play

private:
    void loadThumbnail(QNetworkAccessManager* nam);
    static int chromeHeight(); // fixed height below the thumbnail (title+meta+buttons)

    SearchResult  m_result;
    RoundedThumb* m_thumb;
};

#endif // VIDEOCARD_H
