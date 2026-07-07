#include "PlayerTitleBar.h"
#include "MarqueeLabel.h"
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

    // Long titles elide inside the fixed-width pill and marquee on hover.
    titleLabel = new MarqueeLabel("", pillFrame);
    titleLabel->setObjectName("playerTitleLabel"); // styled by Theme::apply
    titleLabel->setAlignment(Qt::AlignCenter);

    // Left spacer mirrors the far-right X so the title stays truly centred.
    auto* leftSpacer = new QWidget(pillFrame);
    leftSpacer->setFixedSize(20, 20);

    frameLayout->addWidget(leftSpacer);
    frameLayout->addStretch();
    frameLayout->addWidget(titleLabel);
    frameLayout->addWidget(m_spinner); // loading seagull flies just right of the title text
    frameLayout->addStretch();

    // Far-right X: hard-stop + tear down the player.
    m_closeBtn = new QPushButton(QStringLiteral("✕"), pillFrame);
    m_closeBtn->setObjectName("bannerCloseButton"); // themed by Theme::apply
    m_closeBtn->setFixedSize(20, 20);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setToolTip(QStringLiteral("Close player"));
    connect(m_closeBtn, &QPushButton::clicked, this, &PlayerTitleBar::closeRequested);
    frameLayout->addWidget(m_closeBtn);

    setFixedSize(500, 40);
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
