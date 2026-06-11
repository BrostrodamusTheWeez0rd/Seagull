#include "VideoCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QFont>
#include <QFontMetrics>
#include <QApplication>
#include <QStringList>
#include <QPixmapCache>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

namespace {
constexpr int kCardMargin = 8;    // QVBoxLayout content margin (left+right)
constexpr int kVSpace = 6;        // spacing between the card's stacked elements
constexpr int kThumbSrcCap = 720; // widest source thumbnail we keep
// The rounded thumbnail is rendered once at this reference size, then scaled to the
// card on paint. Big enough to stay crisp at any card width; radius is scaled too.
constexpr int kRefThumbW = 480;
constexpr int kRefThumbH = 270;   // 16:9
constexpr int kRefRadius = 18;
int thumbHeightFor(int thumbWidth) { return thumbWidth * 9 / 16; } // 16:9
}

// A 16:9 thumbnail with rounded ("pilled") corners. The image is rounded ONCE at a
// reference resolution; paintEvent just scales that cached pixmap to the widget.
// So when the card grows/shrinks on a window resize there's no re-rounding — only a
// cheap scaled blit — which is what keeps the grid smooth.
class RoundedThumb : public QWidget {
public:
    explicit RoundedThumb(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); // card sets our height
    }

    void setSource(const QPixmap& src) {
        m_rounded = src.isNull() ? QPixmap() : roundedReference(src);
        update();
    }

    // Glyph shown while there's no image (and permanently, if none arrives) —
    // e.g. "♪" for audio files without cover art.
    void setPlaceholderText(const QString& text) {
        m_placeholder = text;
        update();
    }

    QSize minimumSizeHint() const override { return QSize(40, 23); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect r = rect();

        if (m_rounded.isNull()) {
            // Placeholder: a themed rounded tile (palette follows the theme),
            // with the glyph scaled to the tile so a "♪" reads as artwork.
            const int radius = qMin(10, r.height() / 4);
            QPainterPath path;
            path.addRoundedRect(r, radius, radius);
            p.fillPath(path, palette().color(QPalette::AlternateBase));
            p.setPen(palette().color(QPalette::PlaceholderText));
            QFont f = p.font();
            f.setPixelSize(qMax(12, r.height() / 3));
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, m_placeholder);
            return;
        }
        p.drawPixmap(r, m_rounded); // scaled from the reference render
    }

private:
    // Render once: scale to cover, centre-crop to 16:9, round the corners.
    static QPixmap roundedReference(const QPixmap& src) {
        const QSize sz(kRefThumbW, kRefThumbH);
        QPixmap scaled = src.scaled(sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int x = (scaled.width() - sz.width()) / 2;
        const int y = (scaled.height() - sz.height()) / 2;
        scaled = scaled.copy(x, y, sz.width(), sz.height());

        QPixmap out(sz);
        out.fill(Qt::transparent);
        QPainter cp(&out);
        cp.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, sz.width(), sz.height(), kRefRadius, kRefRadius);
        cp.setClipPath(path);
        cp.drawPixmap(0, 0, scaled);
        return out;
    }

    QPixmap m_rounded; // rounded render at reference size
    QString m_placeholder = QStringLiteral("…");
};

VideoCard::VideoCard(const SearchResult& result, QNetworkAccessManager* nam, int cardWidth,
    QWidget* parent, int buttons)
    : QWidget(parent), m_result(result) {
    setObjectName("videoCard"); // themable via Theme::apply's global sheet
    setAttribute(Qt::WA_StyledBackground, true); // let the QSS background/border paint
    setCursor(Qt::PointingHandCursor);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(kCardMargin, kCardMargin, kCardMargin, kCardMargin);
    lay->setSpacing(kVSpace);

    m_thumb = new RoundedThumb(this);
    m_thumb->setObjectName("videoCardThumb");
    lay->addWidget(m_thumb);

    auto* title = new QLabel(m_result.title, this);
    title->setObjectName("videoCardTitle");
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    title->setWordWrap(true);
    title->setFixedHeight(QFontMetrics(tf).height() * 2 + 2); // exactly 2 lines (deterministic)
    lay->addWidget(title);

    QStringList bits;
    if (!m_result.channel.isEmpty()) bits << m_result.channel;
    if (m_result.duration >= 0)      bits << formatDuration(m_result.duration);
    if (m_result.viewCount >= 0)     bits << (formatViewCount(m_result.viewCount) + " views");
    auto* meta = new QLabel(bits.join("   |   "), this);
    meta->setObjectName("metaStats"); // reuse the theme's dimmed stat styling
    meta->setWordWrap(false);         // single line keeps card height deterministic
    lay->addWidget(meta);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(4);

    auto addButton = [&](const QString& label, auto signal) {
        auto* b = new QPushButton(label, this);
        b->setObjectName("videoCardButton");
        b->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(b, 1); // share the card width evenly
        connect(b, &QPushButton::clicked, this, [this, signal]() {
            emit (this->*signal)(QUrl(m_result.url), m_result.title);
            });
        };
    if (buttons & PlayButton)     addButton("▶ Play", &VideoCard::playRequested);
    if (buttons & QueueButton)    addButton("Queue", &VideoCard::queueRequested);
    if (buttons & DownloadButton) addButton("Download", &VideoCard::downloadRequested);
    lay->addLayout(btnRow);

    setCardWidth(cardWidth);
    if (nam) loadThumbnail(nam);
}

void VideoCard::setThumbnail(const QPixmap& pm) {
    m_thumb->setSource(pm);
}

void VideoCard::setThumbnailPlaceholder(const QString& text) {
    m_thumb->setPlaceholderText(text);
}

void VideoCard::setCardWidth(int width) {
    setFixedSize(width, heightForCardWidth(width));
    const int tw = width - 2 * kCardMargin;
    m_thumb->setFixedHeight(thumbHeightFor(tw)); // the thumb just scales its cached render
}

int VideoCard::chromeHeight() {
    static const int cached = [] {
        const QFont base = QApplication::font();
        QFont bold = base; bold.setBold(true);
        const int titleH = QFontMetrics(bold).height() * 2 + 2; // 2 lines
        const int metaH  = QFontMetrics(base).height();
        const int btnH   = QPushButton(QStringLiteral("Ag")).sizeHint().height();
        return 2 * kCardMargin + 3 * kVSpace + titleH + metaH + btnH;
    }();
    return cached;
}

int VideoCard::heightForCardWidth(int cardWidth) {
    return thumbHeightFor(cardWidth - 2 * kCardMargin) + chromeHeight();
}

void VideoCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit playRequested(QUrl(m_result.url), m_result.title);
    QWidget::mousePressEvent(event);
}

void VideoCard::loadThumbnail(QNetworkAccessManager* nam) {
    if (m_result.thumbnail.isEmpty()) return; // RoundedThumb shows its placeholder

    // Process-wide cache keyed by URL so re-running a search doesn't re-download.
    QPixmap cached;
    if (QPixmapCache::find(m_result.thumbnail, &cached)) { m_thumb->setSource(cached); return; }

    QNetworkRequest req((QUrl(m_result.thumbnail)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);
    const QString key = m_result.thumbnail;
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pm;
        if (!pm.loadFromData(reply->readAll())) return;
        if (pm.width() > kThumbSrcCap) pm = pm.scaledToWidth(kThumbSrcCap, Qt::SmoothTransformation);
        QPixmapCache::insert(key, pm);
        m_thumb->setSource(pm);
        });
}

QString VideoCard::formatDuration(qint64 seconds) {
    if (seconds < 0) return QString();
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 s = seconds % 60;
    if (h > 0) return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString VideoCard::formatViewCount(qint64 views) {
    if (views < 0) return QString();
    if (views >= 1000000) return QString::number(views / 1000000.0, 'f', 1) + "M";
    if (views >= 1000)    return QString::number(views / 1000.0, 'f', 1) + "K";
    return QString::number(views);
}
