#include "PlayerControls.h"
#include "../../Backend/PlaybackEngine.h"
#include <QFrame>
#include <QStyle>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
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
#include <QPalette>
#include <cmath>

namespace {
// A slider that jumps to the click position (and lets you keep dragging from
// there), instead of QSlider's default page-step on groove clicks. Works for the
// horizontal seeker and the vertical volume bar (top = max via upsideDown).
class ClickSlider : public QSlider {
public:
    using QSlider::QSlider;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && maximum() > minimum()) {
            const bool vertical = orientation() == Qt::Vertical;
            const int pos  = vertical ? e->pos().y() : e->pos().x();
            const int span = vertical ? height() : width();
            const int v = QStyle::sliderValueFromPosition(
                minimum(), maximum(), pos, span, vertical);
            setValue(v); // moves the handle under the cursor so the base class
            // then enters its normal drag, giving click-then-drag scrubbing.
        }
        QSlider::mousePressEvent(e);
    }
};

// MDI6 volume glyph for the current mute state.
QString volumeIconPath(bool muted) {
    return muted ? QStringLiteral(":/Assets/icons/volume-off.svg")
                 : QStringLiteral(":/Assets/icons/volume-high.svg");
}
}

PlayerControls::PlayerControls(PlaybackEngine* engine, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_duration(0), isUserSeeking(false),
    m_settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat) {

    auto* windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);

    // Icon tints come from the themed palette (see makeIcon/refreshIconTints).
    // Idle = overlay foreground (BrightText: white on dark themes, dark on light);
    // hover = the inverse window colour so the glyph reads against the fill.
    m_iconIdle  = palette().color(QPalette::BrightText);
    m_iconHover = palette().color(QPalette::Window);

    auto* pillFrame = new QFrame(this);
    pillFrame->setObjectName("PillFrame"); // styled by Theme::apply's global sheet
    windowLayout->addWidget(pillFrame);

    auto* mainLayout = new QHBoxLayout(pillFrame);
    mainLayout->setContentsMargins(20, 5, 20, 5);
    mainLayout->setSpacing(2);

    timeLabel = new QLabel("0:00 / 0:00");
    timeLabel->setObjectName("playerTimeLabel"); // styled by Theme::apply
    timeLabel->setMinimumWidth(50);

    positionSlider = new ClickSlider(Qt::Horizontal);
    positionSlider->setObjectName("playerSeekSlider"); // styled by Theme::apply
    positionSlider->setCursor(Qt::PointingHandCursor);

    prevBtn = new QPushButton();
    playPauseBtn = new QPushButton();
    stopBtn = new QPushButton();
    nextBtn = new QPushButton();
    muteBtn = new QPushButton();
    recordBtn = new QPushButton();
    qualityBtn = new QPushButton();
    qualityBtn->hide();
    fullscreenBtn = new QPushButton();

    QSize iconSize(20, 20);
    m_iconButtons = { prevBtn, playPauseBtn, recordBtn, stopBtn, nextBtn, muteBtn, qualityBtn, fullscreenBtn };

    for (auto* btn : m_iconButtons) {
        btn->setObjectName("playerCtlButton"); // styled by Theme::apply
        btn->setIconSize(iconSize);
        btn->installEventFilter(this);
    }

    // Icons are tinted to the theme text colour (the glyph only — the border and
    // hover fill come from the stylesheet, so they stay accent-coloured).
    prevBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/skip-previous.svg"), prevBtn));
    playPauseBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/play.svg"), playPauseBtn));
    stopBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/stop.svg"), stopBtn));
    nextBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/skip-next.svg"), nextBtn));
    qualityBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/cog.svg"), qualityBtn)); // MDI cog, themed like the rest
    fullscreenBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/fullscreen.svg"), fullscreenBtn)); // MDI fullscreen

    // Record button: live-only, hidden until a live stream plays. Themed like the rest
    // when idle (it's in m_iconButtons above); turns red and pulses while recording.
    recordBtn->setToolTip(QStringLiteral("Record stream"));
    recordBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/record.svg"), recordBtn));
    recordBtn->hide();

    mainLayout->addWidget(prevBtn);
    mainLayout->addWidget(playPauseBtn);
    mainLayout->addWidget(recordBtn);
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
    contentFrame->setObjectName("volumePopup"); // styled by Theme::apply
    contentFrame->setGeometry(0, 0, 40, 140);

    auto* volLayout = new QVBoxLayout(contentFrame);
    volLayout->setContentsMargins(10, 15, 10, 15);

    volumeSlider = new ClickSlider(Qt::Vertical);
    volumeSlider->setObjectName("volumeSlider"); // styled by Theme::apply
    volumeSlider->setRange(0, 100);
    volumeSlider->setCursor(Qt::PointingHandCursor);
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
    qualityContentFrame->setObjectName("qualityPopup"); // styled by Theme::apply

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
    connect(recordBtn, &QPushButton::clicked, this, [this]() { emit recordToggleRequested(); });

    // Smoothly pulses the record glyph between dim and bright red while recording
    // (a sine fade, ~1.3s per cycle), so it reads as "recording" rather than blinking.
    recordPulseTimer = new QTimer(this);
    recordPulseTimer->setInterval(50);
    connect(recordPulseTimer, &QTimer::timeout, this, [this]() {
        m_pulsePhase += 0.24;
        const double s = (std::sin(m_pulsePhase) + 1.0) * 0.5; // 0..1
        auto lerp = [](int a, int b, double t) { return int(a + (b - a) * t); };
        retintIcon(recordBtn, QColor(lerp(130, 255, s), lerp(22, 70, s), lerp(22, 70, s)));
        });

    connect(positionSlider, &QSlider::sliderPressed, this, [this]() {
        isUserSeeking = true;
        seek(positionSlider->value()); // jump to a clicked position immediately
        });
    connect(positionSlider, &QSlider::sliderMoved, this, [this](int value) {
        seek(value); // live scrub while dragging
        });
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

void PlayerControls::setLiveMode(bool isLive) {
    m_isLive = isLive;
    // Live still seeks within VLC's DVR window (length()/time() track it), so the
    // slider stays fully live — the only difference is the timestamp shows a LIVE
    // badge in place of a fixed total duration. pollVlcState renders it each tick.
    if (m_isLive) timeLabel->setText(QStringLiteral("● LIVE"));
    updateRecordVisibility(); // record is offered only for live streams
}

void PlayerControls::updateRecordVisibility() {
    const bool show = m_isStream && m_isLive;
    recordBtn->setVisible(show);
    if (!show && m_recording) setRecording(false); // left live -> drop the flashing state
}

void PlayerControls::setRecording(bool on) {
    if (m_recording == on) return;
    m_recording = on;
    if (on) {
        // Keep the record glyph, turn it red and start the pulse.
        recordBtn->setToolTip(QStringLiteral("Stop recording"));
        m_pulsePhase = 0.0;
        retintIcon(recordBtn, QColor(255, 70, 70));
        recordPulseTimer->start();
    } else {
        recordPulseTimer->stop();
        recordBtn->setToolTip(QStringLiteral("Record stream"));
        retintIcon(recordBtn, recordBtn->underMouse() ? m_iconHover : m_iconIdle); // back to themed
    }
}

void PlayerControls::resetUiState() {
    isUserSeeking = false;
    m_duration = -1;
    positionSlider->blockSignals(true);
    positionSlider->setRange(0, 0);
    positionSlider->setValue(0);
    positionSlider->blockSignals(false);
    timeLabel->setText("0:00 / 0:00");
    m_isLive = false;          // new media: live status re-asserted by the probe
    updateRecordVisibility();  // hide record until we know it's live again
    startPolling();
}

void PlayerControls::setStreamingMode(bool isStream) {
    m_isStream = isStream;
    qualityBtn->setVisible(isStream);
    if (!isStream && qualityFrame) qualityFrame->hide();
    updateRecordVisibility();
    startPolling();
}

void PlayerControls::onPrevSingleClick() {
    if (!m_engine) return;
    qint64 newTime = qMax(0LL, m_engine->time() - 5000LL);
    m_engine->setTime(newTime);
}

void PlayerControls::onNextSingleClick() {
    if (!m_engine) return;
    qint64 newTime = qMin(m_engine->length(), m_engine->time() + 5000LL);
    m_engine->setTime(newTime);
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

    if (m_engine) {
        m_engine->setVolume(savedVolume);
        m_engine->setMute(savedMute);
    }

    setVolumeUi(savedVolume);
    if (muteBtn)
        muteBtn->setIcon(makeIcon(volumeIconPath(savedMute), muteBtn));
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

QIcon PlayerControls::tintIcon(const QIcon& src, const QColor& col) const {
    // A stylesheet can't recolour a QIcon, so flat-tint it by painting the colour
    // over the glyph's alpha. Works for standard glyphs and SVG/resource icons.
    QPixmap pm = src.pixmap(QSize(20, 20));
    if (pm.isNull()) return src;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), col);
    p.end();
    return QIcon(pm);
}

QColor PlayerControls::iconColorFor(QPushButton* btn) const {
    return (btn && btn->underMouse()) ? m_iconHover : m_iconIdle;
}

QIcon PlayerControls::makeIcon(const QString& resourcePath, QPushButton* btn) const {
    return tintIcon(QIcon(resourcePath), iconColorFor(btn));
}

void PlayerControls::retintIcon(QPushButton* btn, const QColor& col) {
    // Recolour whatever glyph the button currently shows (preserves its shape).
    if (btn->icon().isNull()) return; // e.g. the mute button while it shows "75%" text
    btn->setIcon(tintIcon(btn->icon(), col));
}

void PlayerControls::refreshIconTints() {
    m_iconIdle  = palette().color(QPalette::BrightText);
    m_iconHover = palette().color(QPalette::Window);
    for (auto* btn : m_iconButtons) {
        if (btn == recordBtn && m_recording) continue; // the pulse owns it
        retintIcon(btn, btn->underMouse() ? m_iconHover : m_iconIdle);
    }
}

void PlayerControls::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // Theme::apply sets a new app palette; re-tint the glyphs to match.
    if (event->type() == QEvent::PaletteChange) refreshIconTints();
}

void PlayerControls::resetMuteButton() {
    if (!this || !muteBtn) return;

    muteBtn->setText("");
    bool isMuted = m_settings.value("Audio/Muted", false).toBool();
    muteBtn->setIcon(makeIcon(volumeIconPath(isMuted), muteBtn));
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
    if (btn && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
        && !(btn == recordBtn && m_recording)) { // the pulse owns the record glyph while recording
        // Re-tint the glyph for hover: idle uses the text colour, hover uses the
        // on-accent colour to read against the accent fill the stylesheet paints.
        retintIcon(btn, event->type() == QEvent::Enter ? m_iconHover : m_iconIdle);
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
    if (m_engine) m_engine->setVolume(volume);
    setVolumeUi(volume);
    m_settings.setValue("Audio/Volume", volume);
}

void PlayerControls::togglePlayback() {
    // After EOF the play button is a replay button — restart from the top.
    if (m_endedMode) { emit replayRequested(); return; }
    if (m_engine->isPlaying()) m_engine->pause();
    else m_engine->play();
}

void PlayerControls::toggleMute() {
    bool isCurrentlyMuted = m_settings.value("Audio/Muted", false).toBool();
    bool willBeMuted = !isCurrentlyMuted;

    if (m_engine) m_engine->setMute(willBeMuted);

    muteBtn->setIcon(makeIcon(volumeIconPath(willBeMuted), muteBtn));
    m_settings.setValue("Audio/Muted", willBeMuted);
}

void PlayerControls::pollVlcState() {
    if (!m_engine) return;

    const PlaybackEngine::State st = m_engine->state();
    const bool isPlaying = (st == PlaybackEngine::State::Playing);

    // Once the stream has ended, freeze the seeker/timestamp where they are and
    // turn the play button into a replay button. (onEndReached sets m_endedMode.)
    if (m_endedMode) {
        playPauseBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/restart.svg"), playPauseBtn));
        return;
    }

    playPauseBtn->setIcon(makeIcon(
        isPlaying ? QStringLiteral(":/Assets/icons/pause.svg") : QStringLiteral(":/Assets/icons/play.svg"),
        playPauseBtn
    ));

    // time()/length() are only trustworthy while actually playing or paused.
    // During Opening/Buffering/Ended they read 0 or stale, which is what makes
    // the seeker and timestamp drift out of sync with the real stream position.
    if (st != PlaybackEngine::State::Playing && st != PlaybackEngine::State::Paused) return;

    qint64 length = m_engine->length();
    if (length > 0 && (m_duration <= 0 || length != m_duration)) {
        m_duration = length;
        positionSlider->setRange(0, static_cast<int>(m_duration));
    }

    qint64 time = m_engine->time();
    if (time >= 0 && !isUserSeeking) {
        positionSlider->blockSignals(true);
        positionSlider->setValue(static_cast<int>(time));
        positionSlider->blockSignals(false);
    }

    // On a live stream there's no fixed end — show the DVR position against a LIVE
    // badge instead of a total. The slider still tracks/seeks within the window.
    if (m_isLive)
        timeLabel->setText(formatTime(time) + QStringLiteral(" / ● LIVE"));
    else
        timeLabel->setText(formatTime(time) + " / " + formatTime(m_duration));
}

void PlayerControls::seek(int position) {
    if (m_engine->length() > 0)
        m_engine->setTime(position);
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
        qb->setObjectName("qualityItem");      // styled by Theme::apply
        qb->setProperty("active", isActive);   // drives the [active="true"] rule
        qb->setCursor(Qt::PointingHandCursor);

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