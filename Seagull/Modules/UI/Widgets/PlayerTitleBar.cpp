#include "PlayerTitleBar.h"
#include <QFrame>
#include <QVBoxLayout>
#include <QLabel>

PlayerTitleBar::PlayerTitleBar(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("pillFrame");
    pillFrame->setStyleSheet(
        "#pillFrame {"
        "background-color: rgba(25, 25, 25, 215);"
        "border-radius: 20px;"
        "border: 1px solid white;"
        "}"
    );

    layout->addWidget(pillFrame);

    auto* frameLayout = new QVBoxLayout(pillFrame);
    frameLayout->setContentsMargins(20, 5, 20, 5);

    titleLabel = new QLabel("", pillFrame);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(
        "QLabel {"
        "color: white;"
        "font-family: 'Segoe UI';"
        "font-size: 14px;"
        "font-weight: bold;"
        "background-color: transparent;"
        "border: none;"
        "}"
    );

    frameLayout->addWidget(titleLabel);

    setFixedSize(500, 40);
}

void PlayerTitleBar::setTitle(const QString& title)
{
    titleLabel->setText(title);
}