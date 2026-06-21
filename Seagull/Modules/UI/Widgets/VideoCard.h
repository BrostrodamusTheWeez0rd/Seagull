#ifndef VIDEOCARD_H
#define VIDEOCARD_H

#include <QWidget>
#include <QUrl>
#include "../../Backend/SgSearch.h" // SearchResult

class QNetworkAccessManager;
class QToolButton;
class RoundedThumb;

// A reusable result tile: thumbnail + title + "channel | length | views", styled
// by the theme. Clicking the thumbnail or the title (or the Play button) asks to
// play the video; the rest of the card doesn't, so the uploader-name link in the
// meta line stays clickable. (Channel cards open the channel instead of playing.)
// The card is sized by the Search grid to fill the row width (grow-to-fill). Its
// thumbnail is rounded once and then *scaled* to the card size, so resizing the
// window never re-renders it — it just stretches a cached pixmap (cheap).
class VideoCard : public QWidget {
    Q_OBJECT
public:
    // Which action buttons the card shows. The Library's local-file cards only
    // need Play; Search results get all three.
    enum CardButton {
        PlayButton     = 0x1,
        QueueButton    = 0x2,
        DownloadButton = 0x4,
        AllButtons     = PlayButton | QueueButton | DownloadButton,
    };

    VideoCard(const SearchResult& result, QNetworkAccessManager* nam, int cardWidth,
        QWidget* parent = nullptr, int buttons = AllButtons,
        const QString& playLabel = {});

    void setCardWidth(int width); // grid-assigned width; thumbnail stays 16:9

    // Hand the card an already-loaded thumbnail (local files: generated frames /
    // cover art, no network involved).
    void setThumbnail(const QPixmap& pm);

    // Glyph drawn in the thumbnail slot until/unless an image arrives
    // (default "…"; the Library uses "♪" for audio).
    void setThumbnailPlaceholder(const QString& text);

    static QString formatDuration(qint64 seconds);
    static QString formatViewCount(qint64 views);
    static QString formatAge(qint64 timestamp); // approx upload time -> "3 weeks ago" ("" if unknown)

    // Card height for a given width (16:9 thumbnail + the fixed chrome below it).
    static int heightForCardWidth(int cardWidth);

signals:
    void playRequested(const QUrl& url, const QString& title);
    void queueRequested(const QUrl& url, const QString& title);
    void downloadRequested(const QUrl& url, const QString& title);
    // The uploader name (video card) or "View Channel" (channel card) was clicked.
    void channelRequested(const QString& channelUrl, const QString& channelName);

protected:
    void mousePressEvent(QMouseEvent* event) override; // click anywhere = play (or open channel)
    void changeEvent(QEvent* event) override;          // PaletteChange -> re-tint star icon

private:
    void loadThumbnail(QNetworkAccessManager* nam);
    void applyThumbnail(QPixmap pm, const QString& cacheKey); // cap + cache + show
    static int chromeHeight(); // fixed height below the thumbnail (title+meta+buttons)

    // Tint an SVG resource path to the given colour; returns a 16x16 pixmap.
    static QPixmap tintSvg(const QString& resourcePath, const QColor& color);
    // Apply the correct star icon (filled/outline) based on current favorite state.
    void updateStarIcon(bool favorited);

    SearchResult  m_result;
    QString       m_channelUrl; // cached from m_result.channelUrl for signal handler
    RoundedThumb* m_thumb;
    QWidget*      m_title    = nullptr; // click-to-play target (with the thumbnail)
    QToolButton*  m_starBtn  = nullptr; // null when not a YouTube card
};

#endif // VIDEOCARD_H
