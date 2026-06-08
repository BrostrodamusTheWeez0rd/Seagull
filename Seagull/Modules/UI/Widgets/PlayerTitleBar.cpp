#include "PlayerTitleBar.h"
#include <QFrame>
#include <QVBoxLayout>
#include <QLabel>

PlayerTitleBar::PlayerTitleBar(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("pillFrame"); // styled by Theme::apply's global sheet

    layout->addWidget(pillFrame);

    auto* frameLayout = new QVBoxLayout(pillFrame);
    frameLayout->setContentsMargins(20, 5, 20, 5);

    titleLabel = new QLabel("", pillFrame);
    titleLabel->setObjectName("playerTitleLabel"); // styled by Theme::apply
    titleLabel->setAlignment(Qt::AlignCenter);

    frameLayout->addWidget(titleLabel);

    setFixedSize(500, 40);
}

void PlayerTitleBar::setTitle(const QString& title)
{
    titleLabel->setText(title);
}