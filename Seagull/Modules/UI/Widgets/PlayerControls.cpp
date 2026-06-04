#include "PlayerControls.h"
#include <QFrame>
#include <QStyle>
#include <QIcon>
#include <QGraphicsColorizeEffect>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QTime>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QMouseEvent>
#include <QApplication>
#include <QHideEvent>
#include <QDebug> 

PlayerControls::PlayerControls(QMediaPlayer* player, QWidget* parent)
    : QWidget(parent), m_player(player), m_duration(0) {

    auto* windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("PillFrame");
    pillFrame->setStyleSheet("QFrame#PillFrame { background-color: rgba(25, 25, 25, 215); border-radius: 25px; border: 1px solid white; }");
    windowLayout->addWidget(pillFrame);

    auto* mainLayout = new QHBoxLayout(pillFrame);
    mainLayout->setContentsMargins(20, 5, 20, 5);
    mainLayout->setSpacing(2);

    timeLabel = new QLabel("0:00 / 0:00");
    timeLabel->setStyleSheet("color: white; font-family: Segoe UI; font-size: 11px;");
    timeLabel->setMinimumWidth(50);

    positionSlider = new QSlider(Qt::Horizontal);
    positionSlider->setCursor(Qt::PointingHandCursor);
    positionSlider->setStyleSheet(
        "QSlider::groove:horizontal { border: none; height: 6px; background: #111; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: white; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: white; width: 12px; margin: -3px 0; border-radius: 6px; border: none; }"
    );

    prevBtn = new QPushButton();
    playPauseBtn = new QPushButton();
    stopBtn = new QPushButton();
    nextBtn = new QPushButton();
    muteBtn = new QPushButton();
    qualityBtn = new QPushButton("⚙");

    // RESTORED: Hide by default until we know it's a stream
    qualityBtn->hide();

    fullscreenBtn = new QPushButton("⛶");

    prevBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    stopBtn->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    nextBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    muteBtn->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));

    QSize iconSize(20, 20);
    QList<QPushButton*> btns = { prevBtn, playPauseBtn, stopBtn, nextBtn, muteBtn, qualityBtn, fullscreenBtn };

    QString btnStyle = "QPushButton { background-color: transparent; border: 1px solid white; border-radius: 15px; min-width: 30px; min-height: 30px; font-size: 16px; color: white; } "
        "QPushButton:hover { background-color: white; color: black; border: 1px solid white; }";

    for (auto* btn : btns) {
        btn->setIconSize(iconSize);
        btn->setStyleSheet(btnStyle);
        auto* effect = new QGraphicsColorizeEffect(btn);
        effect->setColor(Qt::white);
        effect->setStrength(1.0);
        btn->setGraphicsEffect(effect);
        btn->installEventFilter(this);
    }

    mainLayout->addWidget(prevBtn);
    mainLayout->addWidget(playPauseBtn);
    mainLayout->addWidget(stopBtn);
    mainLayout->addWidget(nextBtn);
    mainLayout->addWidget(timeLabel);
    mainLayout->addWidget(positionSlider, 1);
    mainLayout->addWidget(muteBtn);
    mainLayout->addWidget(qualityBtn);
    mainLayout->addWidget(fullscreenBtn);

    setFixedSize(500, 50);

    // --- Vertical Volume Pill Setup ---
    volumeFrame = new QFrame();
    volumeFrame->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    volumeFrame->setAttribute(Qt::WA_TranslucentBackground);
    volumeFrame->setFixedSize(40, 140);

    contentFrame = new QFrame(volumeFrame);
    contentFrame->setGeometry(0, 0, 40, 140);
    contentFrame->setStyleSheet("background: rgba(25, 25, 25, 215); border-radius: 20px; border: 1px solid white;");

    auto* volLayout = new QVBoxLayout(contentFrame);
    volLayout->setContentsMargins(10, 15, 10, 15);

    volumeSlider = new QSlider(Qt::Vertical);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(100);
    volumeSlider->setCursor(Qt::PointingHandCursor);
    volumeSlider->setStyleSheet(
        "QSlider { background: transparent; border: none; }"
        "QSlider::groove:vertical { border: none; background: #111; width: 6px; border-radius: 3px; }"
        "QSlider::add-page:vertical { border: none; background: white; border-radius: 3px; }"
        "QSlider::sub-page:vertical { border: none; background: #333; border-radius: 3px; }"
        "QSlider::handle:vertical { border: none; background: white; height: 10px; margin: 0 -3px; border-radius: 5px; }"
    );
    volLayout->addWidget(volumeSlider);

    volumeHideTimer = new QTimer(this);
    volumeHideTimer->setSingleShot(true);
    connect(volumeHideTimer, &QTimer::timeout, this, &PlayerControls::hideVolumeFrame);

    volumeTextTimer = new QTimer(this);
    volumeTextTimer->setSingleShot(true);
    connect(volumeTextTimer, &QTimer::timeout, this, &PlayerControls::resetMuteButton);

    // --- Vertical Quality Pill Setup (DUMMY UI) ---
    qualityFrame = new QFrame();
    qualityFrame->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    qualityFrame->setAttribute(Qt::WA_TranslucentBackground);

    // Size dynamically based on 3 dummy buttons
    qualityFrame->setFixedSize(60, 110);

    qualityContentFrame = new QFrame(qualityFrame);
    qualityContentFrame->setGeometry(0, 0, 60, 110);
    qualityContentFrame->setStyleSheet("background: rgba(25, 25, 25, 215); border-radius: 20px; border: 1px solid white;");

    auto* qualLayout = new QVBoxLayout(qualityContentFrame);
    qualLayout->setContentsMargins(5, 10, 5, 10);
    qualLayout->setSpacing(2);

    // DUMMY list of buttons just for visuals
    QStringList dummyQualities = { "1080p", "720p", "Audio" };
    for (const QString& q : dummyQualities) {
        QPushButton* qb = new QPushButton(q);
        qb->setCursor(Qt::PointingHandCursor);
        qb->setStyleSheet(
            "QPushButton { background: transparent; color: white; border: none; font-size: 11px; font-weight: bold; border-radius: 5px; padding: 5px; }"
            "QPushButton:hover { background: rgba(255,255,255,50); }"
        );
        connect(qb, &QPushButton::clicked, this, [this, q]() {
            qDebug() << "Dummy quality clicked:" << q;
            qualityFrame->hide();
            });
        qualLayout->addWidget(qb);
    }

    qualityHideTimer = new QTimer(this);
    qualityHideTimer->setSingleShot(true);
    connect(qualityHideTimer, &QTimer::timeout, this, &PlayerControls::hideQualityFrame);

    muteBtn->installEventFilter(this);
    volumeFrame->installEventFilter(this);
    qualityBtn->installEventFilter(this);
    qualityFrame->installEventFilter(this);

    connect(playPauseBtn, &QPushButton::clicked, this, &PlayerControls::togglePlayback);
    connect(stopBtn, &QPushButton::clicked, m_player, &QMediaPlayer::stop);
    connect(muteBtn, &QPushButton::clicked, this, &PlayerControls::toggleMute);
    connect(fullscreenBtn, &QPushButton::clicked, this, &PlayerControls::fullscreenRequested);
    connect(positionSlider, &QSlider::sliderMoved, this, &PlayerControls::seek);
    connect(volumeSlider, &QSlider::valueChanged, this, &PlayerControls::setVolume);

    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &PlayerControls::updateState);
    connect(m_player, &QMediaPlayer::positionChanged, this, &PlayerControls::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &PlayerControls::updateDuration);

    if (m_player->audioOutput()) {
        connect(m_player->audioOutput(), &QAudioOutput::volumeChanged, this, &PlayerControls::syncVolumeUi);
    }
}

void PlayerControls::setStreamingMode(bool isStream) {
    qualityBtn->setVisible(isStream);
    if (!isStream && qualityFrame) {
        qualityFrame->hide();
    }
}

void PlayerControls::hideEvent(QHideEvent* event) {
    if (volumeFrame) volumeFrame->hide();
    if (qualityFrame) qualityFrame->hide();
    QWidget::hideEvent(event);
}

void PlayerControls::syncVolumeUi() {
    if (auto* audio = m_player->audioOutput()) {
        int vol = static_cast<int>(audio->volume() * 100);
        volumeSlider->blockSignals(true);
        volumeSlider->setValue(vol);
        volumeSlider->blockSignals(false);
    }
}

void PlayerControls::updateVolumePosition() {
    if (volumeFrame && volumeFrame->isVisible()) {
        QPoint globalPos = muteBtn->mapToGlobal(QPoint((muteBtn->width() - volumeFrame->width()) / 2, -volumeFrame->height() - 10));
        volumeFrame->move(globalPos);
    }
    if (qualityFrame && qualityFrame->isVisible()) {
        QPoint globalPos = qualityBtn->mapToGlobal(QPoint((qualityBtn->width() - qualityFrame->width()) / 2, -qualityFrame->height() - 10));
        qualityFrame->move(globalPos);
    }
}

void PlayerControls::setVolumeUi(int volume) {
    volumeSlider->blockSignals(true);
    volumeSlider->setValue(volume);
    volumeSlider->blockSignals(false);

    muteBtn->setIcon(QIcon());
    muteBtn->setText(QString::number(volume) + "%");
    volumeTextTimer->start(2000);
}

void PlayerControls::resetMuteButton() {
    muteBtn->setText("");
    bool isMuted = m_player->audioOutput() && m_player->audioOutput()->isMuted();
    muteBtn->setIcon(style()->standardIcon(isMuted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
}

bool PlayerControls::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
        event->accept();
    }

    QPushButton* btn = qobject_cast<QPushButton*>(watched);
    if (btn) {
        auto* effect = static_cast<QGraphicsColorizeEffect*>(btn->graphicsEffect());
        if (effect) {
            if (event->type() == QEvent::Enter) effect->setColor(Qt::black);
            else if (event->type() == QEvent::Leave) effect->setColor(Qt::white);
        }
    }

    // Mutually Exclusive Volume Hover Logic
    if (watched == muteBtn || watched == volumeFrame) {
        if (event->type() == QEvent::Enter) {
            volumeHideTimer->stop();
            if (volumeFrame->isHidden()) {
                qualityFrame->hide();
                qualityHideTimer->stop();

                QPoint globalPos = muteBtn->mapToGlobal(QPoint((muteBtn->width() - volumeFrame->width()) / 2, -volumeFrame->height() - 10));
                volumeFrame->move(globalPos);
                volumeFrame->show();
            }
        }
        else if (event->type() == QEvent::Leave) {
            volumeHideTimer->start(3000);
        }
    }

    // Mutually Exclusive Quality Hover Logic
    if (watched == qualityBtn || watched == qualityFrame) {
        if (event->type() == QEvent::Enter) {
            qualityHideTimer->stop();
            if (qualityFrame->isHidden()) {
                volumeFrame->hide();
                volumeHideTimer->stop();

                QPoint globalPos = qualityBtn->mapToGlobal(QPoint((qualityBtn->width() - qualityFrame->width()) / 2, -qualityFrame->height() - 10));
                qualityFrame->move(globalPos);
                qualityFrame->show();
            }
        }
        else if (event->type() == QEvent::Leave) {
            qualityHideTimer->start(3000);
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PlayerControls::hideVolumeFrame() {
    if (!muteBtn->underMouse() && !volumeFrame->underMouse()) {
        volumeFrame->hide();
    }
}

void PlayerControls::hideQualityFrame() {
    if (!qualityBtn->underMouse() && !qualityFrame->underMouse()) {
        qualityFrame->hide();
    }
}

void PlayerControls::setVolume(int volume) {
    if (auto* audio = m_player->audioOutput()) {
        audio->setVolume(volume / 100.0f);
        setVolumeUi(volume);
    }
}

void PlayerControls::togglePlayback() {
    if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause();
    else m_player->play();
}

void PlayerControls::toggleMute() {
    if (auto* audio = m_player->audioOutput()) {
        bool willBeMuted = !audio->isMuted();
        audio->setMuted(willBeMuted);
        muteBtn->setIcon(style()->standardIcon(willBeMuted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    }
}

void PlayerControls::updateState(QMediaPlayer::PlaybackState state) {
    playPauseBtn->setIcon(style()->standardIcon(
        state == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay
    ));
}

void PlayerControls::updatePosition(qint64 position) {
    if (!positionSlider->isSliderDown()) positionSlider->setValue(static_cast<int>(position));
    timeLabel->setText(formatTime(position) + " / " + formatTime(m_duration));
}

void PlayerControls::updateDuration(qint64 duration) {
    m_duration = duration;
    positionSlider->setRange(0, static_cast<int>(duration));
    timeLabel->setText(formatTime(m_player->position()) + " / " + formatTime(m_duration));
}

void PlayerControls::seek(int position) { m_player->setPosition(position); }

QString PlayerControls::formatTime(qint64 ms) {
    int seconds = (ms / 1000) % 60;
    int minutes = (ms / 60000) % 60;
    int hours = (ms / 3600000);
    QTime time(hours, minutes, seconds);
    return hours > 0 ? time.toString("h:mm:ss") : time.toString("m:ss");
}