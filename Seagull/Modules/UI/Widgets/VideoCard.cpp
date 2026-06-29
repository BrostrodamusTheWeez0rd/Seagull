#include "VideoCard.h"
#include "../../Backend/SgFavorites.h"
#include "../../Backend/SgThumbnailer.h" // decodeViaFfmpeg (WebP avatars)

#include <QDateTime>
#include <QEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
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
#include <QSvgRenderer>

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

    // An SVG glyph drawn (centred, themed) when there's no image — used for the
    // channel avatar fallback (the MDI "account" symbol).
    void setPlaceholderIcon(const QString& svgResource) {
        QSvgRenderer r(svgResource);
        QPixmap pm(128, 128);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        r.render(&p);
        m_placeholderIcon = pm;
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
            if (!m_placeholderIcon.isNull()) {
                // Centre the glyph at ~half the tile's short side.
                const int s = qMax(16, qMin(r.width(), r.height()) / 2);
                const QRect ir(r.center().x() - s / 2, r.center().y() - s / 2, s, s);
                p.drawPixmap(ir, m_placeholderIcon);
                return;
            }
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
    QPixmap m_placeholderIcon; // optional SVG glyph (channel avatar fallback)
};

VideoCard::VideoCard(const SearchResult& result, QNetworkAccessManager* nam, int cardWidth,
    QWidget* parent, int buttons, const QString& playLabel)
    : QWidget(parent), m_result(result) {
    setObjectName("videoCard"); // themable via Theme::apply's global sheet
    setAttribute(Qt::WA_StyledBackground, true); // let the QSS background/border paint
    setCursor(Qt::PointingHandCursor);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(kCardMargin, kCardMargin, kCardMargin, kCardMargin);
    lay->setSpacing(kVSpace);

    m_thumb = new RoundedThumb(this);
    m_thumb->setObjectName("videoCardThumb");
    if (m_result.isChannel) m_thumb->setPlaceholderIcon(QStringLiteral(":/Assets/icons/account.svg"));
    lay->addWidget(m_thumb);

    auto* title = new QLabel(m_result.title, this);
    title->setObjectName("videoCardTitle");
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    title->setWordWrap(true);
    title->setFixedHeight(QFontMetrics(tf).height() * 2 + 2); // exactly 2 lines (deterministic)
    lay->addWidget(title);
    m_title = title; // click-to-play target (with the thumbnail)

    // Meta area: for cards that have a YouTube channel URL we insert a star button
    // beside the channel name. For all other cards the layout is the same single
    // label it always was — just wrapped in a QHBoxLayout so the chrome height
    // calculation stays correct.
    m_channelUrl = m_result.channelUrl;
    // The star routes to the favourites store that owns this URL (YouTube or PornHub);
    // null means the URL belongs to no favouritable site, so no star is shown.
    SgFavorites* const favStore = SgFavorites::forUrl(m_channelUrl);

    auto* metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(0, 0, 0, 0);
    metaRow->setSpacing(2);

    if (favStore) {
        // Star button — flat, icon-only, sits to the right of the channel name.
        // Constructed here, but added to metaRow after the channel label below.
        m_starBtn = new QToolButton(this);
        m_starBtn->setFixedSize(16, 16);
        m_starBtn->setAutoRaise(true);        // flat / no border
        m_starBtn->setCursor(Qt::PointingHandCursor);
        m_starBtn->setToolTip("Favorite channel");
        updateStarIcon(favStore->isFavorited(m_channelUrl));
        connect(m_starBtn, &QToolButton::clicked, this, [this, favStore]() {
            // Pass the thumbnail URL for channel cards (the avatar) — and for Chaturbate
            // rooms, whose card image IS the model, so favourites/home cards aren't blank.
            // Plain video cards carry a frame, not an avatar, so they pass none (the
            // YouTube store fetches a missing avatar via yt-dlp; PornHub leaves it empty).
            const bool keepThumb = m_result.isChannel
                || m_channelUrl.contains("chaturbate.com", Qt::CaseInsensitive);
            favStore->toggle(
                m_channelUrl,
                m_result.channel,
                keepThumb ? m_result.thumbnail : QString());
        });
        // When another card (or any other caller) toggles the same channel, sync up.
        connect(favStore, &SgFavorites::changed, this,
                [this](const QString& url, bool fav) {
                    if (url == m_channelUrl) updateStarIcon(fav);
                });
    }

    if (m_result.isChannel) {
        auto* meta = new QLabel(this);
        meta->setObjectName("metaStats");
        meta->setWordWrap(false);
        meta->setText(!m_result.subscriberText.isEmpty()
            ? m_result.subscriberText
            : (m_result.subscriberCount >= 0
                ? formatViewCount(m_result.subscriberCount) + " subscribers"
                : QStringLiteral("Channel")));
        metaRow->addWidget(meta, 1);
        // Star sits to the right of the subscriber count on channel cards too.
        if (m_starBtn) metaRow->addWidget(m_starBtn);
    } else if (!m_result.channelUrl.isEmpty() && !m_result.channel.isEmpty()) {
        // Uploader name is a clickable link; star sits to its right.
        auto* channelLabel = new QLabel(this);
        channelLabel->setObjectName("metaStats");
        channelLabel->setWordWrap(false);
        channelLabel->setTextFormat(Qt::RichText);
        channelLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        const QString dim = palette().color(QPalette::PlaceholderText).name();
        channelLabel->setText(
            QString("<a href=\"#\" style=\"color:%1;text-decoration:underline;\">%2</a>")
                .arg(dim, m_result.channel.toHtmlEscaped()));
        connect(channelLabel, &QLabel::linkActivated, this, [this](const QString&) {
            emit channelRequested(m_result.channelUrl, m_result.channel);
        });
        metaRow->addWidget(channelLabel);
        // Star to the right of the channel name.
        if (m_starBtn) metaRow->addWidget(m_starBtn);

        QStringList statBits;
        if (m_result.duration >= 0)  statBits << ("   |   " + formatDuration(m_result.duration).toHtmlEscaped());
        if (m_result.viewCount >= 0) statBits << ("   |   " + formatViewCount(m_result.viewCount).toHtmlEscaped() + " views");
        if (const QString age = formatAge(m_result.timestamp); !age.isEmpty()) statBits << ("   |   " + age.toHtmlEscaped());
        if (!statBits.isEmpty()) {
            auto* statsLabel = new QLabel(this);
            statsLabel->setObjectName("metaStats");
            statsLabel->setWordWrap(false);
            statsLabel->setTextFormat(Qt::RichText);
            statsLabel->setText(QString("<span style=\"color:%1;\">%2</span>")
                .arg(dim, statBits.join(QString())));
            metaRow->addWidget(statsLabel);
        }
        metaRow->addStretch(1);
    } else {
        auto* meta = new QLabel(this);
        meta->setObjectName("metaStats");
        meta->setWordWrap(false);
        QStringList bits;
        if (!m_result.channel.isEmpty()) bits << m_result.channel;
        if (m_result.duration >= 0)      bits << formatDuration(m_result.duration);
        if (m_result.viewCount >= 0)     bits << (formatViewCount(m_result.viewCount) + " views");
        if (const QString age = formatAge(m_result.timestamp); !age.isEmpty()) bits << age;
        meta->setText(bits.join("   |   "));
        metaRow->addWidget(meta, 1);
    }

    lay->addLayout(metaRow);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(4);

    if (m_result.isChannel) {
        // Channel card: a single action that opens the channel's video page.
        auto* b = new QPushButton(QStringLiteral("View Channel"), this);
        b->setObjectName("videoCardButton");
        b->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(b, 1);
        connect(b, &QPushButton::clicked, this, [this]() {
            emit channelRequested(m_result.channelUrl, m_result.channel);
        });
    } else {
        auto addButton = [&](const QString& label, auto signal) {
            auto* b = new QPushButton(label, this);
            b->setObjectName("videoCardButton");
            b->setCursor(Qt::PointingHandCursor);
            btnRow->addWidget(b, 1); // share the card width evenly
            connect(b, &QPushButton::clicked, this, [this, signal]() {
                emit (this->*signal)(QUrl(m_result.url), m_result.title);
                });
            };
        if (buttons & PlayButton)     addButton(playLabel.isEmpty() ? QStringLiteral("▶ Play") : playLabel, &VideoCard::playRequested);
        if (buttons & QueueButton)    addButton("Queue", &VideoCard::queueRequested);
        if (buttons & DownloadButton) addButton("Download", &VideoCard::downloadRequested);
    }
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
    // Click-to-play is limited to the thumbnail and the title, so the rest of the
    // card (especially the uploader link, which sits just above the buttons) stays
    // clickable for its own action. The full-card hover highlight is unchanged.
    if (event->button() == Qt::LeftButton) {
        const QWidget* hit = childAt(event->position().toPoint());
        if (hit == m_thumb || hit == m_title) {
            if (m_result.isChannel) emit channelRequested(m_result.channelUrl, m_result.channel);
            else                    emit playRequested(QUrl(m_result.url), m_result.title);
        }
    }
    QWidget::mousePressEvent(event);
}

void VideoCard::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange && m_starBtn) {
        if (SgFavorites* favStore = SgFavorites::forUrl(m_channelUrl))
            updateStarIcon(favStore->isFavorited(m_channelUrl));
    }
    QWidget::changeEvent(event);
}

QPixmap VideoCard::tintSvg(const QString& resourcePath, const QColor& color) {
    // Render the SVG to a 16x16 pixmap then paint the target colour over its alpha.
    QSvgRenderer renderer(resourcePath);
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), color);
    p.end();
    return pm;
}

void VideoCard::updateStarIcon(bool favorited) {
    if (!m_starBtn) return;
    const QColor color = favorited
        ? palette().color(QPalette::Highlight)
        : palette().color(QPalette::PlaceholderText);
    const QString path = favorited
        ? QStringLiteral(":/Assets/icons/star.svg")
        : QStringLiteral(":/Assets/icons/star-outline.svg");
    m_starBtn->setIcon(QIcon(tintSvg(path, color)));
    m_starBtn->setIconSize(QSize(14, 14));
}

void VideoCard::loadThumbnail(QNetworkAccessManager* nam) {
    if (m_result.thumbnail.isEmpty()) return; // RoundedThumb shows its placeholder

    // Process-wide cache keyed by URL so re-running a search doesn't re-download.
    QPixmap cached;
    if (QPixmapCache::find(m_result.thumbnail, &cached)) { m_thumb->setSource(cached); return; }

    QNetworkRequest req((QUrl(m_result.thumbnail)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    // Some CDNs (PornHub's CDN77) hotlink-protect thumbnails and 403 without a
    // site Referer; the parser supplies it for those results.
    if (!m_result.thumbnailReferer.isEmpty())
        req.setRawHeader("Referer", m_result.thumbnailReferer.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);
    const QString key = m_result.thumbnail;
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QByteArray data = reply->readAll();
        QPixmap pm;
        if (pm.loadFromData(data)) { applyThumbnail(pm, key); return; }
        // QPixmap couldn't decode it — e.g. a WebP channel avatar (this Qt build
        // ships no qwebp plugin). Round-trip the bytes through ffmpeg to PNG.
        SgThumbnailer::decodeViaFfmpeg(data, this, [this, key](const QPixmap& decoded) {
            applyThumbnail(decoded, key);
        });
        });
}

void VideoCard::applyThumbnail(QPixmap pm, const QString& cacheKey) {
    if (pm.isNull()) return; // leave the placeholder / channel glyph in place
    if (pm.width() > kThumbSrcCap) pm = pm.scaledToWidth(kThumbSrcCap, Qt::SmoothTransformation);
    if (!cacheKey.isEmpty()) QPixmapCache::insert(cacheKey, pm);
    m_thumb->setSource(pm);
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

QString VideoCard::formatAge(qint64 timestamp) {
    if (timestamp < 0) return QString();
    const qint64 secs = QDateTime::currentSecsSinceEpoch() - timestamp;
    if (secs < 0) return QString();
    // yt-dlp's approximate_date is derived from YouTube's relative "X ago" text, so
    // mirror that wording (an exact date would imply precision it doesn't have).
    struct Unit { qint64 secs; const char* one; const char* many; };
    static const Unit units[] = {
        { 31536000, "year",  "years"  },
        {  2592000, "month", "months" },
        {   604800, "week",  "weeks"  },
        {    86400, "day",   "days"   },
        {     3600, "hour",  "hours"  },
        {       60, "minute","minutes"},
    };
    for (const auto& u : units) {
        const qint64 n = secs / u.secs;
        if (n >= 1) return QString("%1 %2 ago").arg(n).arg(n == 1 ? u.one : u.many);
    }
    return QStringLiteral("just now");
}
