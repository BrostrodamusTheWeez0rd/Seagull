#include "PlayerTitleBar.h"
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMovie>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPalette>
#include <QEvent>

PlayerTitleBar::PlayerTitleBar(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("pillFrame"); // styled by Theme::apply's global sheet

    layout->addWidget(pillFrame);

    auto* frameLayout = new QHBoxLayout(pillFrame);
    frameLayout->setContentsMargins(14, 5, 14, 5);
    frameLayout->setSpacing(8);

    // Animated seagull, scaled by height to keep the landscape gif's aspect ratio.
    m_movie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), this);
    m_movie->jumpToFrame(0);
    const QSize frame = m_movie->currentPixmap().size();
    const int spinH = 22;
    const int spinW = frame.height() > 0 ? frame.width() * spinH / frame.height() : spinH;
    m_movie->setScaledSize(QSize(spinW, spinH));
    m_spinner = new QLabel(pillFrame);
    m_spinner->setMovie(m_movie);
    m_spinner->hide();

    titleLabel = new QLabel("", pillFrame);
    titleLabel->setObjectName("playerTitleLabel"); // styled by Theme::apply
    titleLabel->setAlignment(Qt::AlignCenter);

    // Info / Share actions (icons tinted to overlayFg; shown only for online streams).
    infoBtn = new QPushButton(pillFrame);
    infoBtn->setObjectName("bannerActionButton");
    infoBtn->setIconSize(QSize(18, 18));
    infoBtn->setCursor(Qt::PointingHandCursor);
    infoBtn->setToolTip("Video info");
    infoBtn->hide();
    connect(infoBtn, &QPushButton::clicked, this, &PlayerTitleBar::infoRequested);

    shareBtn = new QPushButton(pillFrame);
    shareBtn->setObjectName("bannerActionButton");
    shareBtn->setIconSize(QSize(18, 18));
    shareBtn->setCursor(Qt::PointingHandCursor);
    shareBtn->setToolTip("Copy link");
    shareBtn->hide();
    connect(shareBtn, &QPushButton::clicked, this, &PlayerTitleBar::shareRequested);

    refreshActionIcons();

    frameLayout->addStretch();
    frameLayout->addWidget(titleLabel);
    frameLayout->addWidget(m_spinner); // loading seagull flies just right of the title text
    frameLayout->addStretch();
    frameLayout->addWidget(infoBtn);   // actions pinned to the right of the banner
    frameLayout->addWidget(shareBtn);

    setFixedSize(500, 40);
}

void PlayerTitleBar::refreshActionIcons()
{
    // SVG glyphs flat-tinted to the theme's overlay foreground (a stylesheet can't
    // recolour an icon). Matches the control-bar buttons' look.
    const QColor col = palette().color(QPalette::BrightText);
    auto tint = [&](const QString& path) -> QIcon {
        QPixmap pm = QIcon(path).pixmap(QSize(18, 18));
        if (pm.isNull()) return QIcon(path);
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), col);
        p.end();
        return QIcon(pm);
    };
    if (infoBtn)  infoBtn->setIcon(tint(":/Assets/icons/info.svg"));
    if (shareBtn) shareBtn->setIcon(tint(":/Assets/icons/share.svg"));
}

void PlayerTitleBar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) refreshActionIcons();
}

void PlayerTitleBar::setTitle(const QString& title)
{
    titleLabel->setText(title);
}

void PlayerTitleBar::setLoading(bool loading)
{
    if (!m_spinner || !m_movie) return;
    if (loading) {
        m_movie->start();
        m_spinner->show();
    } else {
        m_spinner->hide();
        m_movie->stop();
    }
}

void PlayerTitleBar::setActionsVisible(bool on)
{
    if (infoBtn)  infoBtn->setVisible(on);
    if (shareBtn) shareBtn->setVisible(on);
}
