#include "PlayerControls.h"
#include "../../Backend/PlaybackEngine.h"
#include "../../Backend/SgPaths.h"
#include "ClickSlider.h"
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
#include <QWheelEvent>
#include <QApplication>
#include <QCursor>
#include <QHideEvent>
#include <QGraphicsOpacityEffect>
#include <QVariantAnimation>
#include <QDebug>
#include <QSettings>
#include <QCoreApplication>
#include <QPalette>
#include <cmath>

namespace {
// ClickSlider (jump-to-click) now lives in Widgets/ClickSlider.h, shared with the EQ tab.

// MDI6 volume glyph for the current mute state.
QString volumeIconPath(bool muted) {
    return muted ? QStringLiteral(":/Assets/icons/volume-off.svg")
                 : QStringLiteral(":/Assets/icons/volume-high.svg");
}
}

PlayerControls::PlayerControls(PlaybackEngine* engine, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_duration(0), isUserSeeking(false),
    m_settings(SgPaths::configFile(), QSettings::IniFormat) {

    // Icon tints come from the themed palette (see makeIcon/refreshIconTints).
    // Idle = overlay foreground (BrightText: white on dark themes, dark on light);
    // hover = the inverse window colour so the glyph reads against the fill.
    m_iconIdle  = palette().color(QPalette::BrightText);
    m_iconHover = palette().color(QPalette::Window);

    // The pill is NOT in a fill layout — it's positioned manually inside the window
    // (see layoutBar) so seek growth can slide/stretch it without resizing the
    // translucent top-level window every frame.
    m_pillFrame = new QFrame(this);
    m_pillFrame->setObjectName("PillFrame"); // styled by Theme::apply's global sheet
    QFrame* pillFrame = m_pillFrame;         // local alias for the setup below

    auto* mainLayout = new QHBoxLayout(pillFrame);
    mainLayout->setContentsMargins(20, 5, 20, 5);
    mainLayout->setSpacing(2);

    timeLabel = new QLabel("0:00 / 0:00");
    timeLabel->setObjectName("playerTimeLabel"); // styled by Theme::apply
    timeLabel->setMinimumWidth(50);

    positionSlider = new ClickSlider(Qt::Horizontal);
    positionSlider->setObjectName("playerSeekSlider"); // styled by Theme::apply
    positionSlider->setCursor(Qt::PointingHandCursor);
    positionSlider->installEventFilter(this); // hover grows the bar (see eventFilter)
    positionSlider->setMouseTracking(true);   // deliver hover moves for the time tooltip

    // Time tooltip — a translucent top-level container (same idiom as the volume /
    // quality popups) wrapping the styled pill label, so the corners stay clean over
    // the video. A bare translucent QLabel won't paint its stylesheet background.
    seekTooltip = new QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    seekTooltip->setAttribute(Qt::WA_TranslucentBackground);
    seekTooltip->setAttribute(Qt::WA_ShowWithoutActivating);
    seekTooltipText = new QLabel(seekTooltip);
    seekTooltipText->setObjectName("seekTooltip"); // styled by Theme::apply
    seekTooltipText->setAlignment(Qt::AlignCenter);
    seekTooltip->hide();

    prevBtn = new QPushButton();
    playPauseBtn = new QPushButton();
    nextBtn = new QPushButton();
    muteBtn = new QPushButton();
    recordBtn = new QPushButton();
    qualityBtn = new QPushButton();
    qualityBtn->hide();
    popoutBtn = new QPushButton();
    visualizerBtn = new QPushButton();
    visualizerBtn->hide(); // audio-only; shown via setVisualizerMode
    // Small, flat triangle buttons that flank the visualizer button to cycle
    // through visualizers (prev / next). Audio-only, shown via setVisualizerMode.
    prevVizBtn = new QPushButton(QStringLiteral("◂")); // ◂
    nextVizBtn = new QPushButton(QStringLiteral("▸")); // ▸
    for (auto* b : { prevVizBtn, nextVizBtn }) {
        b->setObjectName("vizNavButton"); // small circle, themed in Theme.cpp
        b->setFixedHeight(22);
        b->setFixedWidth(0);              // collapsed -> no gap; width animates open
        b->setCursor(Qt::PointingHandCursor);
        b->installEventFilter(this);      // hover-reveal (see eventFilter)
        b->hide();
    }
    // Reveal by animating WIDTH (0 -> 22) so the triangles smoothly push the
    // neighbouring controls apart, plus an opacity fade. Collapsed + hidden when
    // out, so there's no constant gap.
    m_prevVizFx = new QGraphicsOpacityEffect(prevVizBtn);
    m_nextVizFx = new QGraphicsOpacityEffect(nextVizBtn);
    prevVizBtn->setGraphicsEffect(m_prevVizFx);
    nextVizBtn->setGraphicsEffect(m_nextVizFx);
    m_prevVizFx->setOpacity(0.0);
    m_nextVizFx->setOpacity(0.0);
    m_vizFade = new QVariantAnimation(this);
    m_vizFade->setDuration(160);
    connect(m_vizFade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        const qreal o = v.toReal();
        const int w = qRound(22.0 * o);
        prevVizBtn->setFixedWidth(w);
        nextVizBtn->setFixedWidth(w);
        if (m_prevVizFx) m_prevVizFx->setOpacity(o);
        if (m_nextVizFx) m_nextVizFx->setOpacity(o);
        // Viz growth widens the bar symmetrically about its centre (see layoutBar).
        m_vizExtra = w; // each side adds a triangle of this width
        layoutBar();
    });
    connect(m_vizFade, &QVariantAnimation::finished, this, [this]() {
        if (m_prevVizFx && m_prevVizFx->opacity() <= 0.01) { // fully collapsed -> drop the space
            prevVizBtn->setVisible(false);
            nextVizBtn->setVisible(false);
        }
    });
    prevVizBtn->setToolTip(QStringLiteral("Previous visualizer"));
    nextVizBtn->setToolTip(QStringLiteral("Next visualizer"));
    fullscreenBtn = new QPushButton();

    QSize iconSize(20, 20);
    m_iconButtons = { prevBtn, playPauseBtn, recordBtn, nextBtn, muteBtn, qualityBtn, popoutBtn, visualizerBtn, fullscreenBtn };

    for (auto* btn : m_iconButtons) {
        btn->setObjectName("playerCtlButton"); // styled by Theme::apply
        btn->setIconSize(iconSize);
        btn->installEventFilter(this);
    }

    // Icons are tinted to the theme text colour (the glyph only — the border and
    // hover fill come from the stylesheet, so they stay accent-coloured).
    prevBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/skip-previous.svg"), prevBtn));
    playPauseBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/play.svg"), playPauseBtn));
    nextBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/skip-next.svg"), nextBtn));
    qualityBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/cog.svg"), qualityBtn)); // MDI cog, themed like the rest
    popoutBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/popout.svg"), popoutBtn)); // MDI open-in-new
    popoutBtn->setToolTip(QStringLiteral("Pop out into its own window"));
    fullscreenBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/fullscreen.svg"), fullscreenBtn)); // MDI fullscreen
    visualizerBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/visualizer.svg"), visualizerBtn));
    visualizerBtn->setToolTip(QStringLiteral("Visualizer"));

    // Record button: live-only, hidden until a live stream plays. Themed like the rest
    // when idle (it's in m_iconButtons above); turns red and pulses while recording.
    recordBtn->setToolTip(QStringLiteral("Record stream"));
    recordBtn->setIcon(makeIcon(QStringLiteral(":/Assets/icons/record.svg"), recordBtn));
    recordBtn->hide();

    mainLayout->addWidget(prevBtn);
    mainLayout->addWidget(playPauseBtn);
    mainLayout->addWidget(recordBtn);
    mainLayout->addWidget(nextBtn);
    mainLayout->addWidget(timeLabel);
    mainLayout->addWidget(positionSlider, 1);
    mainLayout->addWidget(muteBtn);
    mainLayout->addWidget(qualityBtn);
    mainLayout->addWidget(popoutBtn);
    mainLayout->addWidget(prevVizBtn);
    mainLayout->addWidget(visualizerBtn);
    mainLayout->addWidget(nextVizBtn);
    mainLayout->addWidget(fullscreenBtn);

    // Height fixed; width driven explicitly (m_baseWidth / layoutBar). The pill fills
    // the window; setBaseWidth re-sizes it for the chosen Progress bar size.
    setFixedHeight(50);
    setMinimumWidth(m_baseWidth);
    setMaximumWidth(QWIDGETSIZE_MAX);
    resize(m_baseWidth, 50);
    m_pillFrame->setGeometry(0, 0, m_baseWidth, 50);

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
    connect(muteBtn, &QPushButton::clicked, this, &PlayerControls::toggleMute);
    connect(fullscreenBtn, &QPushButton::clicked, this, &PlayerControls::fullscreenRequested);
    connect(visualizerBtn, &QPushButton::clicked, this, &PlayerControls::visualizerRequested);
    connect(prevVizBtn, &QPushButton::clicked, this, [this]() { emit visualizerCycleRequested(-1); });
    connect(nextVizBtn, &QPushButton::clicked, this, [this]() { emit visualizerCycleRequested(+1); });

    // Cycle triangles hover-reveal: shown while the cursor is on the visualizer
    // button or a triangle (only when the visualizer is on); a short grace delay
    // lets the cursor travel from the button onto a triangle without them vanishing.
    vizTriHideTimer = new QTimer(this);
    vizTriHideTimer->setSingleShot(true);
    vizTriHideTimer->setInterval(300); // grace so you can reach a triangle
    connect(vizTriHideTimer, &QTimer::timeout, this, [this]() {
        if (visualizerBtn->underMouse() || prevVizBtn->underMouse() || nextVizBtn->underMouse())
            vizTriHideTimer->start();      // still hovered -> keep them up
        else fadeVizTriangles(false);
    });
    connect(popoutBtn, &QPushButton::clicked, this, &PlayerControls::popoutRequested);
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
        // Pause while scrubbing (resume on release) so the engine isn't fighting
        // our setTime calls — keeps the timestamp/handle from drifting.
        if (m_engine && m_engine->isPlaying() && !m_endedMode) {
            m_resumeAfterSeek = true;
            m_engine->pause();
        }
        seek(positionSlider->value()); // jump to a clicked position immediately
        showSeekTooltipAtHandle(positionSlider->value());
        });
    connect(positionSlider, &QSlider::sliderMoved, this, [this](int value) {
        seek(value); // live scrub while dragging
        showSeekTooltipAtHandle(value); // pill tracks the handle
        });
    connect(positionSlider, &QSlider::sliderReleased, this, [this]() {
        isUserSeeking = false;
        seek(positionSlider->value());
        if (m_resumeAfterSeek) { m_resumeAfterSeek = false; if (m_engine) m_engine->play(); }
        if (seekTooltip) seekTooltip->hide();
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
}

void PlayerControls::setRecordAvailable(bool avail) {
    // Offered for any playing media — live stream, VOD, or local file (the player picks
    // the right capture method). Hidden when playback is torn down.
    recordBtn->setVisible(avail);
    if (!avail && m_recording) setRecording(false); // playback gone -> drop the pulse
}

void PlayerControls::setPoppedOut(bool popped) {
    // Swap the glyph/tooltip so the same button reads as "pop out" while docked
    // and "return to the main window" while floating.
    popoutBtn->setIcon(makeIcon(popped ? QStringLiteral(":/Assets/icons/popin.svg")
                                       : QStringLiteral(":/Assets/icons/popout.svg"), popoutBtn));
    popoutBtn->setToolTip(popped ? QStringLiteral("Return to the main window")
                                 : QStringLiteral("Pop out into its own window"));
}

void PlayerControls::setVisualizerMode(bool on) {
    // Audio: show the visualizer toggle button (fullscreen stays too). The cycle
    // triangles stay collapsed (zero width, hidden = no gap) and expand on hover.
    m_visualizerMode = on;
    visualizerBtn->setVisible(on);
    if (!on) {
        m_vizFade->stop();
        prevVizBtn->setFixedWidth(0); nextVizBtn->setFixedWidth(0);
        m_prevVizFx->setOpacity(0.0);  m_nextVizFx->setOpacity(0.0);
        prevVizBtn->setVisible(false); nextVizBtn->setVisible(false);
        m_vizExtra = 0; layoutBar(); // collapse the triangles' width contribution
    }
}

void PlayerControls::setVisualizerActive(bool on) {
    m_vizActive = on;
    if (on) {
        // Just turned on with the cursor on the button -> reveal triangles now.
        if (m_visualizerMode && visualizerBtn->underMouse()) fadeVizTriangles(true);
    } else {
        fadeVizTriangles(false);
    }
}

void PlayerControls::layoutBar() {
    // Window == pill. Viz triangles widen it symmetrically about the locked home so
    // revealing them doesn't drift the controls off-centre.
    const int pillW = m_baseWidth + 2 * m_vizExtra;
    if (width() != pillW)
        setGeometry(m_barCenterX - pillW / 2, y(), pillW, height());
    m_pillFrame->setGeometry(0, 0, pillW, height());
}

int PlayerControls::widthForSize(const QString& size) {
    // Small == the original 500px bar; larger sizes hand the seeker more pixels.
    if (size == QStringLiteral("Large"))  return 860;
    if (size == QStringLiteral("Medium")) return 680;
    return 500; // "Small" / default
}

void PlayerControls::setBaseWidth(int width) {
    // The Progress bar size setting (Settings -> Display) maps to a control pill width;
    // a wider pill gives the seeker more pixels (finer scrubbing). The parent re-centres
    // the bar over the video (VideoPlayer::repositionOverlays) right after this.
    if (width <= 0 || width == m_baseWidth) return;
    m_baseWidth = width;
    const int pillW = m_baseWidth + 2 * m_vizExtra;
    setMinimumWidth(m_baseWidth);
    resize(pillW, height());
    m_pillFrame->setGeometry(0, 0, pillW, height());
}

void PlayerControls::moveSeekTooltip(int sliderX, qint64 valueMs) {
    if (!seekTooltip || !seekTooltipText) return;
    // On a live stream the right end is the live edge — you can't scrub past it, so
    // sitting at (or within ~1.5s of) it reads as LIVE rather than a DVR offset.
    const bool atLiveEdge = m_isLive && (positionSlider->maximum() - valueMs) <= 1500;
    seekTooltipText->setText(atLiveEdge ? QStringLiteral("● LIVE") : formatTime(valueMs));
    seekTooltipText->adjustSize();
    seekTooltip->resize(seekTooltipText->size());   // container hugs the styled pill
    seekTooltipText->move(0, 0);
    // sliderX is in the seeker's coordinate space; sit the pill just above the bar,
    // centred on that x. mapToGlobal keeps it correct docked or popped out.
    const QPoint g = positionSlider->mapToGlobal(QPoint(sliderX, 0));
    seekTooltip->move(g.x() - seekTooltip->width() / 2, g.y() - seekTooltip->height() - 8);
    if (!seekTooltip->isVisible()) seekTooltip->show();
    seekTooltip->raise();
}

void PlayerControls::showSeekTooltipAtCursor(int cursorX) {
    // Hover: show the time a click here would seek to, centred on the cursor.
    if (!positionSlider || positionSlider->maximum() <= positionSlider->minimum()) return;
    const int span = positionSlider->width();
    const int x = qBound(0, cursorX, span);
    const int v = QStyle::sliderValueFromPosition(
        positionSlider->minimum(), positionSlider->maximum(), x, span, false);
    moveSeekTooltip(x, v);
}

void PlayerControls::showSeekTooltipAtHandle(int value) {
    // Drag: lock the pill to the handle centre (QSS handle width = 12px -> 6px half).
    if (!positionSlider || positionSlider->maximum() <= positionSlider->minimum()) return;
    const int hw = 6;
    const int travel = positionSlider->width() - 2 * hw;
    const double t = double(value - positionSlider->minimum()) /
                     double(positionSlider->maximum() - positionSlider->minimum());
    moveSeekTooltip(hw + qRound(t * travel), value);
}

void PlayerControls::fadeVizTriangles(bool in) {
    if (!m_vizFade || !m_prevVizFx) return;
    const qreal cur = m_prevVizFx->opacity();
    if ((in && cur >= 1.0) || (!in && cur <= 0.0)) return; // already there
    if (in) {
        prevVizBtn->setVisible(true);
        nextVizBtn->setVisible(true);
        m_barCenterX = x() + width() / 2; // lock the centre (invariant under symmetric growth)
    }
    m_vizFade->stop();
    m_vizFade->setStartValue(cur);
    m_vizFade->setEndValue(in ? 1.0 : 0.0);
    m_vizFade->start();
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
    startPolling();
}

void PlayerControls::setStreamingMode(bool isStream) {
    qualityBtn->setVisible(isStream);
    if (!isStream && qualityFrame) qualityFrame->hide();
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

bool PlayerControls::popupUnderCursor() const {
    const QPoint gp = QCursor::pos();
    if (volumeFrame && volumeFrame->isVisible() && volumeFrame->geometry().contains(gp)) return true;
    if (qualityFrame && qualityFrame->isVisible() && qualityFrame->geometry().contains(gp)) return true;
    return false;
}

void PlayerControls::closePopups() {
    if (volumeFrame)  volumeFrame->hide();
    if (qualityFrame) qualityFrame->hide();
}

void PlayerControls::hideEvent(QHideEvent* event) {
    if (volumeFrame)  volumeFrame->hide();
    if (qualityFrame) qualityFrame->hide();
    if (seekTooltip)  seekTooltip->hide();
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

    if (watched == positionSlider) {
        if (event->type() == QEvent::Leave && !isUserSeeking) {
            if (seekTooltip) seekTooltip->hide();
        }
        else if (event->type() == QEvent::MouseMove && !isUserSeeking) {
            showSeekTooltipAtCursor(static_cast<QMouseEvent*>(event)->pos().x());
        }
    }

    QPushButton* btn = qobject_cast<QPushButton*>(watched);
    if (btn && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
        && !(btn == recordBtn && m_recording)) { // the pulse owns the record glyph while recording
        // Re-tint the glyph for hover: idle uses the text colour, hover uses the
        // on-accent colour to read against the accent fill the stylesheet paints.
        retintIcon(btn, event->type() == QEvent::Enter ? m_iconHover : m_iconIdle);
    }

    // Hover-reveal the cycle triangles when the visualizer is on and the cursor is
    // on the visualizer button or a triangle.
    if (watched == visualizerBtn || watched == prevVizBtn || watched == nextVizBtn) {
        if (event->type() == QEvent::Enter && m_vizActive && m_visualizerMode) {
            vizTriHideTimer->stop();
            fadeVizTriangles(true);
        } else if (event->type() == QEvent::Leave) {
            vizTriHideTimer->start();
        }
    }

    if (watched == muteBtn || watched == volumeFrame) {
        if (event->type() == QEvent::Wheel) {
            // Scrolling the volume button (or the popup body) steps the volume;
            // the slider chain applies it to the engine and persists it.
            const int dy = static_cast<QWheelEvent*>(event)->angleDelta().y();
            if (dy != 0 && volumeSlider)
                volumeSlider->setValue(qBound(0, volumeSlider->value() + (dy > 0 ? 5 : -5), 100));
            return true;
        }
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
            // Quick exit — and always quicker than the controls' own 3s OSD
            // timeout, so a popup is never left hanging past its bar.
            volumeHideTimer->start(800);
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
            qualityHideTimer->start(800); // same quick exit as the volume popup
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

void PlayerControls::nudgeVolume(int delta) {
    if (!volumeSlider) return;
    // Setting the slider's value fires valueChanged -> setVolume (engine + UI + save),
    // exactly like the volume-popup wheel handler.
    volumeSlider->setValue(qBound(0, volumeSlider->value() + delta, 100));
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