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
#include <QMouseEvent>
#include <QApplication>
#include <QCursor>
#include <QHideEvent>
#include <QDebug>
#include <QSettings>
#include <QCoreApplication>

PlayerControls::PlayerControls(VLC::MediaPlayer* player, QWidget* parent)
    : QWidget(parent), m_player(player), m_duration(0), isUserSeeking(false),
    m_settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat) {

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
    qualityBtn->hide();
    fullscreenBtn = new QPushButton("⛶");

    prevBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    stopBtn->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    nextBtn->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));

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

    // --- Volume popup ---
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

    // --- Quality popup ---
    qualityFrame = new QFrame();
    qualityFrame->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    qualityFrame->setAttribute(Qt::WA_TranslucentBackground);

    qualityContentFrame = new QFrame(qualityFrame);
    qualityContentFrame->setStyleSheet("background: rgba(25, 25, 25, 215); border-radius: 20px; border: 1px solid white;");

    auto* qualLayout = new QVBoxLayout(qualityContentFrame);
    qualLayout->setContentsMargins(5, 10, 5, 10);
    qualLayout->setSpacing(2);

    qualityHideTimer = new QTimer(this);
    qualityHideTimer->setSingleShot(true);
    connect(qualityHideTimer, &QTimer::timeout, this, &PlayerControls::hideQualityFrame);

    muteBtn->installEventFilter(this);
    volumeFrame->installEventFilter(this);
    qualityBtn->installEventFilter(this);
    qualityFrame->installEventFilter(this);

    prevClickTimer = new QTimer(this);
    prevClickTimer->setSingleShot(true);
    connect(prevClickTimer, &QTimer::timeout, this, &PlayerControls::onPrevSingleClick);

    nextClickTimer = new QTimer(this);
    nextClickTimer->setSingleShot(true);
    connect(nextClickTimer, &QTimer::timeout, this, &PlayerControls::onNextSingleClick);

    connect(playPauseBtn, &QPushButton::clicked, this, &PlayerControls::togglePlayback);
    connect(stopBtn, &QPushButton::clicked, this, [this]() { emit stopRequested(); });
    connect(muteBtn, &QPushButton::clicked, this, &PlayerControls::toggleMute);
    connect(fullscreenBtn, &QPushButton::clicked, this, &PlayerControls::fullscreenRequested);

    connect(positionSlider, &QSlider::sliderPressed, this, [this]() { isUserSeeking = true; });
    connect(positionSlider, &QSlider::sliderReleased, this, [this]() {
        isUserSeeking = false;
        seek(positionSlider->value());
        });
    connect(volumeSlider, &QSlider::valueChanged, this, &PlayerControls::setVolume);

    uiPollTimer = new QTimer(this);
    connect(uiPollTimer, &QTimer::timeout, this, &PlayerControls::pollVlcState);
    uiPollTimer->start(250);

    applyAudioState();
}

PlayerControls::~PlayerControls() {
    stopPolling();
    if (volumeHideTimer)  volumeHideTimer->stop();
    if (volumeTextTimer)  volumeTextTimer->stop();
    if (qualityHideTimer) qualityHideTimer->stop();
    if (prevClickTimer)   prevClickTimer->stop();
    if (nextClickTimer)   nextClickTimer->stop();
}

void PlayerControls::setCurrentFormat(const QString& formatId) {
    m_currentFormatId = formatId;
    // Force a UI refresh if we have options cached
    if (!m_lastOptions.isEmpty()) {
        setAvailableQualities(m_lastOptions);
    }
}

void PlayerControls::stopPolling() {
    if (uiPollTimer) uiPollTimer->stop();
}

void PlayerControls::startPolling() {
    m_endedMode = false; // new media — let the seeker track again
    if (uiPollTimer && !uiPollTimer->isActive()) {
        uiPollTimer->start(250);
    }
}

void PlayerControls::setEndedMode(bool ended) {
    m_endedMode = ended;
}

void PlayerControls::resetUiState() {
    isUserSeeking = false;
    m_duration = -1;
    positionSlider->blockSignals(true);
    positionSlider->setRange(0, 0);
    positionSlider->setValue(0);
    positionSlider->blockSignals(false);
    timeLabel->setText("0:00 / 0:00");
    startPolling();
}

void PlayerControls::setStreamingMode(bool isStream) {
    qualityBtn->setVisible(isStream);
    if (!isStream && qualityFrame) qualityFrame->hide();
    startPolling();
}

void PlayerControls::onPrevSingleClick() {
    if (!m_player) return;
    qint64 newTime = qMax(0LL, m_player->time() - 5000LL);
    m_player->setTime(newTime);
}

void PlayerControls::onNextSingleClick() {
    if (!m_player) return;
    qint64 newTime = qMin(m_player->length(), m_player->time() + 5000LL);
    m_player->setTime(newTime);
}

void PlayerControls::applyAudioState() {
    if (!m_settings.contains("Audio/Volume")) {
        m_settings.setValue("Audio/Volume", 100);
        m_settings.sync();
    }
    if (!m_settings.contains("Audio/Muted")) {
        m_settings.setValue("Audio/Muted", false);
        m_settings.sync();
    }

    int savedVolume = m_settings.value("Audio/Volume", 100).toInt();
    bool savedMute = m_settings.value("Audio/Muted", false).toBool();

    if (m_player) {
        m_player->setVolume(savedVolume);
        m_player->setMute(savedMute);
    }

    setVolumeUi(savedVolume);
    if (muteBtn)
        muteBtn->setIcon(style()->standardIcon(savedMute ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
}

bool PlayerControls::hasOpenPopup() const {
    return (volumeFrame && volumeFrame->isVisible()) || (qualityFrame && qualityFrame->isVisible());
}

void PlayerControls::hideEvent(QHideEvent* event) {
    if (volumeFrame)  volumeFrame->hide();
    if (qualityFrame) qualityFrame->hide();
    QWidget::hideEvent(event);
}

void PlayerControls::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    updateVolumePosition();
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
    if (!volumeSlider || !muteBtn || !volumeTextTimer) return;

    volumeSlider->blockSignals(true);
    volumeSlider->setValue(volume);
    volumeSlider->blockSignals(false);

    muteBtn->setIcon(QIcon());
    muteBtn->setText(QString::number(volume) + "%");
    volumeTextTimer->start(2000);
}

void PlayerControls::resetMuteButton() {
    if (!this || !muteBtn) return;

    muteBtn->setText("");
    bool isMuted = m_settings.value("Audio/Muted", false).toBool();
    muteBtn->setIcon(style()->standardIcon(isMuted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
}

bool PlayerControls::eventFilter(QObject* watched, QEvent* event) {
    if (watched == prevBtn) {
        if (event->type() == QEvent::MouseButtonPress) {
            prevClickTimer->start(QApplication::doubleClickInterval());
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            prevClickTimer->stop();
            emit skipRequested(-1);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) return true;
    }

    if (watched == nextBtn) {
        if (event->type() == QEvent::MouseButtonPress) {
            nextClickTimer->start(QApplication::doubleClickInterval());
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            nextClickTimer->stop();
            emit skipRequested(+1);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) return true;
    }

    QPushButton* btn = qobject_cast<QPushButton*>(watched);
    if (btn) {
        auto* effect = static_cast<QGraphicsColorizeEffect*>(btn->graphicsEffect());
        if (effect) {
            if (event->type() == QEvent::Enter) effect->setColor(Qt::black);
            else if (event->type() == QEvent::Leave) effect->setColor(Qt::white);
        }
    }

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
    // Use the popup's global geometry so hovering the slider (a child of the
    // frame) still counts as "over the popup" — underMouse() on the parent is
    // false while a child has the cursor, which would hide it mid-use.
    const QPoint gp = QCursor::pos();
    const bool overMute = muteBtn->rect().contains(muteBtn->mapFromGlobal(gp));
    if (!overMute && !volumeFrame->geometry().contains(gp)) volumeFrame->hide();
}

void PlayerControls::hideQualityFrame() {
    const QPoint gp = QCursor::pos();
    const bool overBtn = qualityBtn->rect().contains(qualityBtn->mapFromGlobal(gp));
    if (!overBtn && !qualityFrame->geometry().contains(gp)) qualityFrame->hide();
}

void PlayerControls::setVolume(int volume) {
    if (m_player) m_player->setVolume(volume);
    setVolumeUi(volume);
    m_settings.setValue("Audio/Volume", volume);
}

void PlayerControls::togglePlayback() {
    // After EOF the play button is a replay button — restart from the top.
    if (m_endedMode) { emit replayRequested(); return; }
    if (m_player->isPlaying()) m_player->pause();
    else m_player->play();
}

void PlayerControls::toggleMute() {
    bool isCurrentlyMuted = m_settings.value("Audio/Muted", false).toBool();
    bool willBeMuted = !isCurrentlyMuted;

    if (m_player) m_player->setMute(willBeMuted);

    muteBtn->setIcon(style()->standardIcon(willBeMuted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    m_settings.setValue("Audio/Muted", willBeMuted);
}

void PlayerControls::pollVlcState() {
    if (!m_player) return;

    libvlc_state_t st = m_player->state();
    bool isPlaying = (st == libvlc_Playing);

    // Once the stream has ended, freeze the seeker/timestamp where they are and
    // turn the play button into a replay button. (onEndReached sets m_endedMode.)
    if (m_endedMode) {
        playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        return;
    }

    playPauseBtn->setIcon(style()->standardIcon(
        isPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay
    ));

    // time()/length() are only trustworthy while actually playing or paused.
    // During Opening/Buffering/Ended they read 0 or stale, which is what makes
    // the seeker and timestamp drift out of sync with the real stream position.
    if (st != libvlc_Playing && st != libvlc_Paused) return;

    qint64 length = m_player->length();
    if (length > 0 && (m_duration <= 0 || length != m_duration)) {
        m_duration = length;
        positionSlider->setRange(0, static_cast<int>(m_duration));
    }

    qint64 time = m_player->time();
    if (time >= 0 && !isUserSeeking) {
        positionSlider->blockSignals(true);
        positionSlider->setValue(static_cast<int>(time));
        positionSlider->blockSignals(false);
    }

    timeLabel->setText(formatTime(time) + " / " + formatTime(m_duration));
}

void PlayerControls::seek(int position) {
    if (m_player->length() > 0)
        m_player->setTime(position);
}

QString PlayerControls::formatTime(qint64 ms) {
    if (ms < 0) ms = 0;
    int seconds = (ms / 1000) % 60;
    int minutes = (ms / 60000) % 60;
    int hours = (ms / 3600000);
    QTime time(hours, minutes, seconds);
    return hours > 0 ? time.toString("h:mm:ss") : time.toString("m:ss");
}

void PlayerControls::setAvailableQualities(const QList<StreamOption>& options) {
    m_lastOptions = options; // Cache the list
    if (!qualityContentFrame) return;

    QLayout* layout = qualityContentFrame->layout();
    if (!layout) return;

    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }

    qualityBtn->setVisible(!options.isEmpty());

    for (const StreamOption& opt : options) {
        bool isActive = (opt.formatId == m_currentFormatId);
        QPushButton* qb = new QPushButton(isActive ? (opt.label + " ✓") : opt.label);
        qb->setCursor(Qt::PointingHandCursor);

        QString style = "QPushButton { background: transparent; color: white; border: none; font-size: 11px; ";
        style += (isActive ? "font-weight: bold; border-left: 3px solid white; padding-left: 5px;" : "padding-left: 8px;");
        style += " } QPushButton:hover { background: rgba(255,255,255,50); }";

        qb->setStyleSheet(style);

        connect(qb, &QPushButton::clicked, this, [this, id = opt.formatId]() {
            qualityFrame->hide();
            setCurrentFormat(id);
            emit qualitySelected(id);
            });
        layout->addWidget(qb);
        qb->show();
    }

    int newHeight = (options.size() * 30) + 20;
    if (newHeight < 40) newHeight = 40;

    qualityFrame->setFixedSize(80, newHeight);
    qualityContentFrame->setGeometry(0, 0, 80, newHeight);

    updateVolumePosition();
}