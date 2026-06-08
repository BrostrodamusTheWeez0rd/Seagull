#include "PlayerTitleBar.h"
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMovie>

PlayerTitleBar::PlayerTitleBar(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("pillFrame"); // styled by Theme::apply's global sheet

    layout->addWidget(pillFrame);

    auto* frameLayout = new QHBoxLayout(pillFrame);
    frameLayout->setContentsMargins(20, 5, 20, 5);
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

    frameLayout->addStretch();
    frameLayout->addWidget(m_spinner);
    frameLayout->addWidget(titleLabel);
    frameLayout->addStretch();

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
