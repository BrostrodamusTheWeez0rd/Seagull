#include "VideoCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QMouseEvent>
#include <QFont>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

namespace {
constexpr int kCardWidth = 300;
constexpr int kThumbW = 300;
constexpr int kThumbH = 169; // 16:9
}

VideoCard::VideoCard(const SearchResult& result, QNetworkAccessManager* nam, QWidget* parent)
    : QWidget(parent), m_result(result) {
    setObjectName("videoCard"); // themable via Theme::apply's global sheet
    setFixedWidth(kCardWidth);
    setCursor(Qt::PointingHandCursor);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    m_thumb = new QLabel(this);
    m_thumb->setObjectName("videoCardThumb");
    m_thumb->setFixedSize(kThumbW, kThumbH);
    m_thumb->setAlignment(Qt::AlignCenter);
    m_thumb->setText("…");
    lay->addWidget(m_thumb);

    auto* title = new QLabel(m_result.title, this);
    title->setObjectName("videoCardTitle");
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    title->setWordWrap(true);
    lay->addWidget(title);

    QStringList bits;
    if (!m_result.channel.isEmpty()) bits << m_result.channel;
    if (m_result.duration >= 0)      bits << formatDuration(m_result.duration);
    if (m_result.viewCount >= 0)     bits << (formatViewCount(m_result.viewCount) + " views");
    auto* meta = new QLabel(bits.join("   |   "), this);
    meta->setObjectName("metaStats"); // reuse the theme's dimmed stat styling
    meta->setWordWrap(true);
    lay->addWidget(meta);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(4);

    auto* playBtn = new QPushButton("▶ Play", this);
    auto* queueBtn = new QPushButton("Queue", this);
    auto* downloadBtn = new QPushButton("Download", this);
    for (QPushButton* b : { playBtn, queueBtn, downloadBtn }) {
        b->setObjectName("videoCardButton");
        b->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(b, 1); // share the card width evenly
    }
    lay->addLayout(btnRow);

    connect(playBtn, &QPushButton::clicked, this, [this]() {
        emit playRequested(QUrl(m_result.url), m_result.title);
        });
    connect(queueBtn, &QPushButton::clicked, this, [this]() {
        emit queueRequested(QUrl(m_result.url), m_result.title);
        });
    connect(downloadBtn, &QPushButton::clicked, this, [this]() {
        emit downloadRequested(QUrl(m_result.url), m_result.title);
        });

    if (nam) loadThumbnail(nam);
}

void VideoCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit playRequested(QUrl(m_result.url), m_result.title);
    QWidget::mousePressEvent(event);
}

void VideoCard::loadThumbnail(QNetworkAccessManager* nam) {
    if (m_result.thumbnail.isEmpty()) { m_thumb->setText("No thumbnail"); return; }
    QNetworkRequest req((QUrl(m_result.thumbnail)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { m_thumb->setText("No thumbnail"); return; }
        QPixmap pm;
        if (!pm.loadFromData(reply->readAll())) { m_thumb->setText("No thumbnail"); return; }
        m_thumb->setPixmap(pm.scaled(m_thumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
