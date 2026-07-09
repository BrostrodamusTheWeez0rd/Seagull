#include "Visualizer.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QTimer>
#include <QShowEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QRandomGenerator>
#include <QImageReader>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
    constexpr int kFps      = 60;
    constexpr int kClouds   = 5;
    qreal frand(qreal lo, qreal hi) {
        return lo + (hi - lo) * QRandomGenerator::global()->generateDouble();
    }
    qreal ease(qreal cur, qreal target, qreal k) { return cur + (target - cur) * k; }
    // Smooth-but-snappy follower: fast attack toward a rising target (resolves
    // transients), slower release on the way down (smooth, no jitter).
    qreal easeAR(qreal cur, qreal target, qreal attack, qreal release) {
        return cur + (target - cur) * (target > cur ? attack : release);
    }

    // Swoop shape: a committed dive reaching full depth kSwoopBottom of the way
    // through, then a long slow climb back out — NOT a symmetric dip. The dive is
    // cubic-smoothstep (committed); the climb is a QUINTIC ease, so the gull
    // arrives back at its exact original height with zero velocity AND zero
    // acceleration — it settles onto its line with a float, not a mechanical
    // stop. Returns depth 0..1; optionally writes d(depth)/dp for the pitch tilt.
    constexpr qreal kPi = 3.14159265358979;

    // Wave travel speeds (widths/s at 120 BPM, scaled live by the tempo but
    // CAPPED at kWaveVMax so fast songs never turn the sea into a river). The
    // three flows are deliberately spread apart — each sheet owns its current.
    // [0]=treble (back, quickest chop), [1]=mids, [2]=bass (front, slow swells).
    constexpr qreal kWaveV[3]  = { 0.33, 0.25, 0.19 };
    constexpr qreal kWaveVMax  = 1.75; // cap on the tempo multiplier for water
    constexpr qreal kWaveSpan  = 1.20; // the surface field spans 0.20 off-left .. right edge
    // Alternating dance (user's idea): every second stretch of water rides the
    // live band while the stretch between just cruises, and each layer's
    // pattern is staggered so the pulse plays across the three sheets instead
    // of the whole sea breathing as one block.
    constexpr qreal kDanceFreq     = 24.0; // rad/width: ~every-second-wave alternation
    constexpr qreal kDancePhase[3] = { 0.0, 2.1, 4.2 };
    // The WAVE GENERATOR: a raised-cosine mound centred just off the left edge
    // (its tail straddles the screen edge, so a birth shows the instant it
    // happens rather than after an off-screen transit). It is held at each
    // band's LIVE level, so the water leaving it is the dance itself.
    constexpr qreal kGenX  = -0.04;
    constexpr qreal kGenHw =  0.16; // generator half-width (raised cosine), in widths

    constexpr qreal kSwoopBottom = 0.36; // fraction of the swoop spent diving — a touch quicker down than the climb out (longer dive = more room for a slow open)
    // The gull gif is 5 frames (0..4). A swoop HOLDS frame 0 (wings tucked) all the
    // way down; near the bottom it opens the wings FORWARD only, from the dive frame
    // to the outstretched glide (0 -> kGlideHoldFrame, passing through the frames
    // between so it reads as a smooth flap, not a snap), timed to FINISH a hair past
    // the bottom, then HOLDS that spread until the swoop ends. No full flap.
    constexpr int   kGlideHoldFrame = 2;    // outstretched glide (slight dihedral) — the held pose; the open plays 0..this
    constexpr qreal kFlapSlow       = 2.4;  // >1 slows/smooths the open (its lead grows so it still finishes at the bottom)
    constexpr qreal kFlapLate       = 0.05; // finish the open this far (in swoopP) PAST the arc's bottom, so the glide isn't reached early
    constexpr qreal kFlapMaxDive    = 0.9;  // cap: the open may fill at most this much of the descent (keeps a little tuck, always lands on time)
    qreal swoopDepth(qreal p, qreal* slope = nullptr) {
        if (p < kSwoopBottom) {
            const qreal t = p / kSwoopBottom;
            if (slope) *slope = 6.0 * t * (1.0 - t) / kSwoopBottom;
            return t * t * (3.0 - 2.0 * t);
        }
        const qreal u = (p - kSwoopBottom) / (1.0 - kSwoopBottom);
        if (slope) *slope = -30.0 * u * u * (1.0 - u) * (1.0 - u) / (1.0 - kSwoopBottom);
        return 1.0 - u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    }

    // Horizontal speed multiplier through a swoop (a boost ADDED over the base glide
    // via multiply): it ACCELERATES into the dive so the down-swoop is fast and
    // covers ground — an arc, not a vertical drop — peaks at the bottom, then bleeds
    // off slowly up the far side, a slower glide that still carries the speed it
    // built dropping in. kSwoopDiveBoost = the extra at the bottom; kSwoopGlideCarry
    // = how much of it survives to the top of the climb.
    constexpr qreal kSwoopDiveBoost  = 2.4;
    constexpr qreal kSwoopGlideCarry = 0.55;
    qreal swoopSurge(qreal p) {
        if (p < 0.0) return 1.0;
        if (p < kSwoopBottom) {
            const qreal t = p / kSwoopBottom;                          // 0..1 down
            // Ramp in fast from the very start so the STEEP middle of the drop still
            // carries forward speed — that's what bends the plunge into an arc.
            return 1.0 + kSwoopDiveBoost * t;                          // build to the peak at the bottom
        }
        const qreal u = (p - kSwoopBottom) / (1.0 - kSwoopBottom);     // 0..1 up
        return 1.0 + kSwoopDiveBoost * (kSwoopGlideCarry + (1.0 - kSwoopGlideCarry) * (1.0 - u * u)); // carry, then bleed
    }

    // Cubic smoothstep, clamped: eases 0..1 with zero slope at both ends.
    qreal smooth01(qreal x) { x = qBound(0.0, x, 1.0); return x * x * (3.0 - 2.0 * x); }
    qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * t; }

    // --- Seagull Cycle schedule (fractions of the song) ----------------------
    // The day is a LOOP: it walks Morning -> Day -> Dusk -> Night and back to
    // Morning, so a song ends at the time of day it began and the next song plays
    // as the next day (endless cycling). Hold plateaus + blend bands; all tunable.
    constexpr qreal kMornHoldEnd   = 0.08; // dawn holds, then blends to Day
    constexpr qreal kDayHoldStart  = 0.18;
    constexpr qreal kDayHoldEnd    = 0.33; // Day (noon) holds, then blends to Dusk
    constexpr qreal kDuskHoldStart = 0.44;
    constexpr qreal kDuskHoldEnd   = 0.52; // Dusk (sunset) holds, then blends to Night
    constexpr qreal kNightHoldStart= 0.64;
    constexpr qreal kNightHoldEnd  = 0.84; // Night (midnight) holds, then blends BACK to Morning by tod=1
    constexpr qreal kReactFloor    = 0.12; // min music-reactive warmth (Day / deep Night)
    // Nightfall (stars/beam/dark gulls) rises across dusk, holds, then eases back
    // to zero by dawn so the loop closes cleanly.
    constexpr qreal kNightRiseStart = 0.46;
    constexpr qreal kNightRiseEnd   = 0.62;
    constexpr qreal kNightFallStart = 0.84;
}

Visualizer::Visualizer(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false); // we paint an opaque sky ourselves
    // One random scene layout per session: seedScenery replays it at every size, so
    // a resize rescales the same hills/trees/rocks/stars instead of rolling new ones.
    m_sceneSeed = QRandomGenerator::global()->generate();
    m_timer = new QTimer(this);
    m_timer->setInterval(1000 / kFps);
    connect(m_timer, &QTimer::timeout, this, &Visualizer::step);
    loadGullFrames();
}

void Visualizer::loadGullFrames() {
    // Decode the animated gull once: mirrored to face the drift direction (the
    // gif faces left), downscaled (gulls are small), capturing each frame's
    // native display time so we play it at the gif's own speed.
    QImageReader reader(QStringLiteral(":/Assets/SeagullAnim.gif"));
    for (int guard = 0; guard < 300; ++guard) {
        const QImage img = reader.read();
        if (img.isNull()) break;
        QImage rgba = img.convertToFormat(QImage::Format_ARGB32);
        rgba = rgba.scaledToHeight(160, Qt::SmoothTransformation); // natural facing (left); flipped per direction at draw
        m_gullFrames.push_back(QPixmap::fromImage(rgba));
        // A night-shaded copy of the frame: the same gull pushed toward a dark
        // blue silhouette, for flying in the dark (the beam lights the normal
        // frame back in over it).
        QImage shaded = rgba;
        {
            QPainter dp(&shaded);
            dp.setCompositionMode(QPainter::CompositionMode_SourceAtop);
            dp.fillRect(shaded.rect(), QColor(18, 24, 40, 170));
        }
        m_gullFramesDark.push_back(QPixmap::fromImage(shaded));
        const int d = reader.nextImageDelay();
        m_frameDelays.push_back(d > 0 ? d : 60); // sane default if unspecified
    }
}

void Visualizer::setAudioLevel(float level01) {
    m_tLevel = qBound(0.0, double(level01), 1.0);
}

void Visualizer::setSpectrum(float bass, float mid, float treble) {
    m_tBass   = qBound(0.0, double(bass),   1.0);
    m_tMid    = qBound(0.0, double(mid),    1.0);
    m_tTreble = qBound(0.0, double(treble), 1.0);
}

void Visualizer::beat() {
    // Tempo from the gaps between bass beats — but raw gaps are noisy: a missed
    // kick reads as 2x the true interval, a double-fire as 0.5x, and syncopation
    // lands anywhere. So: octave-FOLD each gap onto the current estimate (those
    // errors then count as evidence for the SAME tempo instead of yanking it),
    // take the median of a recent window (one stray gap can't move a median),
    // then ease. Locks in ~4 beats and stays planted through fills and breaks.
    if (m_lastBeatT >= 0.0) {
        qreal iv = m_t - m_lastBeatT;
        if (iv > 0.20 && iv < 2.2) {
            // Fold only CLEAR octave errors (~2x / ~0.5x). The thresholds are wide
            // enough that a genuine tempo change (even 1.5x slower) passes through
            // raw and re-anchors the estimate instead of being folded forever.
            while (iv > m_beatInterval * 1.75) iv *= 0.5; // missed beat(s): fold down
            while (iv < m_beatInterval * 0.55) iv *= 2.0; // double-fire:     fold up
            if (iv > 0.28 && iv < 1.6) {                  // musical range ~40-210 BPM
                constexpr int kTempoWindow = 7;
                if (m_beatIvals.size() < kTempoWindow) m_beatIvals.push_back(iv);
                else m_beatIvals[m_beatIvalIdx] = iv;
                m_beatIvalIdx = (m_beatIvalIdx + 1) % kTempoWindow;
                QVector<qreal> sorted = m_beatIvals;
                std::sort(sorted.begin(), sorted.end());
                m_beatInterval += (sorted[sorted.size() / 2] - m_beatInterval) * 0.25;
            }
        }
    }
    m_lastBeatT = m_t;
    // Exaggerate the deviation from ~120 BPM so slow vs fast songs glide visibly
    // differently (most songs cluster near 120, so the raw spread is small).
    const qreal raw = 0.5 / m_beatInterval; // 1.0 at ~120 BPM
    m_tempoSpeed = qBound(0.60, 1.0 + (raw - 1.0) * 2.0, 2.4);

    // Phase-lock the lighthouse optic: pull the plan rotation toward the nearest
    // beat-grid phase ALIGNED to the flash (a beam facing the viewer = pi/2, mod
    // pi) so the flash lands ON a beat — not between beats. The grid spacing is
    // the per-beat advance (pi/m_beamBeats); the pull is spread over the next
    // fraction of a second in step(), never a snap.
    const qreal stepA = kPi / m_beamBeats;
    qreal perr = std::fmod(m_beamA - kPi * 0.5, stepA);
    if (perr < 0) perr += stepA;
    if (perr > stepA * 0.5) perr -= stepA;
    m_beamNudge = -perr * 0.35;

    if (!m_dying) { // a beat sends an extra gull across (dying ones don't count)
        int living = 0;
        for (const Gull& g : m_gulls) if (!g.dying) ++living;
        if (living < m_maxGulls) {
            Gull g; recycleGull(g);
            m_gulls.push_back(g);
        }
    }
}

void Visualizer::setDemoMode(bool on) { m_demo = on; m_demoBeatT = 0.0; }

void Visualizer::setBehavior(const QString& name) {
    GullBehavior b = GullBehavior::Drift;
    if (name.contains(QStringLiteral("Reverse"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("Right"),   Qt::CaseInsensitive)) b = GullBehavior::Reverse;
    else if (name.contains(QStringLiteral("Swoop"), Qt::CaseInsensitive)) b = GullBehavior::Swooping;
    else if (name.contains(QStringLiteral("Flock"), Qt::CaseInsensitive)) b = GullBehavior::Flocking;
    if (b != m_behavior) {
        m_behavior = b;
        // The active flock adapts IN PLACE — no respawn. A direction change turns
        // the birds around where they fly, Flocking starts steering the existing
        // gulls toward the band, and any swoop in progress finishes on its own
        // (step/draw run swoops by swoopP, not by the current behaviour).
    }
    update();
}

void Visualizer::setMaxGulls(int n) { m_maxGulls = qBound(1, n, 30); }
void Visualizer::setMode(const QString& name) {
    // "Cycle" runs the full-day arc (m_mode is ignored while cycling). Anything
    // else is a fixed scene: unrecognised (including a legacy "Seagull Sky")
    // lands on Morning; "Waves" is the pre-rename name for Dusk.
    m_cycle = name.contains(QStringLiteral("Cycle"), Qt::CaseInsensitive);
    if (!m_cycle) {
        if      (name.contains(QStringLiteral("Night"), Qt::CaseInsensitive)) m_mode = Mode::Night;
        else if (name.contains(QStringLiteral("Dusk"),  Qt::CaseInsensitive) ||
                 name.contains(QStringLiteral("Waves"), Qt::CaseInsensitive)) m_mode = Mode::Dusk;
        else if (name.contains(QStringLiteral("Day"),   Qt::CaseInsensitive)) m_mode = Mode::Day;
        else                                                                  m_mode = Mode::Morning;
    }
    update();
}

void Visualizer::setProgress(qint64 posMs, qint64 durMs) {
    // Song position -> time of day for Cycle. Unknown/zero duration (live streams)
    // holds at dawn rather than dividing by zero. The 60fps timer repaints; no
    // update() needed here.
    m_todProgress = (durMs > 0) ? qBound(0.0, double(posMs) / double(durMs), 1.0) : 0.0;
}

Visualizer::ScenePalette Visualizer::paletteFor(Mode m) const {
    ScenePalette s;
    switch (m) {
    case Mode::Night:
        s.night = true;
        s.skyTop = QColor("#050912"); s.skyBot = QColor("#13293e");
        s.grass  = QColor("#0a1a20"); s.sand = QColor("#161e2e"); s.trees = QColor("#081420");
        s.sandShadowA = 130;
        s.waterBack = QColor("#0b2a3c"); s.waterMid = QColor("#103a4e"); s.waterFront = QColor("#175066");
        s.crestShimmer = QColor("#b9d7fa"); // cool moonlight on the crests
        break;
    case Mode::Dusk:
        // Sunset over the sea, REACTIVE — its OWN dusk palette, distinct from
        // Morning's soft sunrise: a deep indigo-purple zenith through a plum high
        // sky and a hot magenta-rose boundary into a burnt-orange amber horizon.
        // The water body stays a muted twilight violet; the sunset itself lands as
        // a warm shimmer glinting off the wave crests (crestShimmer).
        s.reactiveSky = true;
        s.skyEqTop  = QColor("#1e1440"); s.skyEqHigh = QColor("#6a2e72");
        s.skyEqRose = QColor("#d1466e"); s.skyEqAmb  = QColor("#f0743a");
        s.skyEqGold = QColor("#ffc25c");
        s.grass  = QColor("#3e5f4d"); s.sand = QColor("#d9b184"); s.trees = QColor("#243f38");
        s.sandShadowA = 120;
        s.waterBack = QColor("#2a2c4c"); s.waterMid = QColor("#3f4064"); s.waterFront = QColor("#585a7e");
        break;
    case Mode::Morning:
        // Sunrise EQ sky over a bright, day-lit shore. The water is a calm dawn
        // blue; the sunrise reflection lands as a soft warm shimmer on the crests.
        s.reactiveSky = true;
        s.skyEqTop  = QColor("#1a2350"); s.skyEqHigh = QColor("#5b3f87");
        s.skyEqRose = QColor("#c75c87"); s.skyEqAmb  = QColor("#f0975a");
        s.skyEqGold = QColor("#ffd79a");
        s.grass  = QColor("#4e8266"); s.sand = QColor("#c6b489"); s.trees = QColor("#2f6152");
        s.sandShadowA = 120;
        s.waterBack = QColor("#274a63"); s.waterMid = QColor("#3f6b82"); s.waterFront = QColor("#6690a0");
        break;
    case Mode::Day:
    default:
        // Bright blue afternoon, static sky — a clear azure up top easing to a
        // light, hazy horizon.
        s.skyTop = QColor("#1e5e97"); s.skyBot = QColor("#8ccbe0");
        s.grass  = QColor("#4e8266"); s.sand = QColor("#c6b489"); s.trees = QColor("#2f6152");
        s.sandShadowA = 120;
        s.waterBack = QColor("#16566e"); s.waterMid = QColor("#1f7d92"); s.waterFront = QColor("#3aa6b3");
        break;
    }
    return s;
}
void Visualizer::setLighthouseBeats(int n) { m_beamBeats = qBound(1, n, 16); }

void Visualizer::setPaused(bool on) {
    m_paused = on;
    updateTimerState();
}

void Visualizer::suspendRendering(bool on) {
    if (m_suspended == on) return;
    m_suspended = on; // e.g. while the Library builds its card grid on the GUI thread
    updateTimerState();
}

void Visualizer::updateTimerState() {
    // Animate only when the sky is actually on screen and nothing wants it frozen.
    if (isVisible() && !m_paused && !m_suspended) m_timer->start();
    else                                          m_timer->stop();
}

void Visualizer::triggerDeath() {
    // The track ended: every gull tips into a spin-and-fall.
    if (m_dying) return;
    m_dying = true;
    const qreal h = qMax(1, height());
    for (Gull& g : m_gulls) {
        g.dying = true;
        g.spin  = frand(4.0, 11.0) * (QRandomGenerator::global()->bounded(2) ? 1.0 : -1.0); // deg/frame
        g.vy    = -frand(0.004, 0.018) * h; // a small upward lurch before gravity takes over
    }
}

void Visualizer::reviveGulls() {
    // Resume spawning living gulls. We DON'T clear the flock: any gulls already
    // dying keep falling out (they finish their death animation) and don't count
    // toward the cap, while fresh living gulls spawn in for the new track.
    m_dying = false;
    m_spawnT = 0.0;
    // New track: reset the tempo tracker to a neutral anchor so the new song gets
    // a clean lock (folding against the OLD track's interval could trap a slower
    // song at double-time forever).
    m_beatIvals.clear();
    m_beatIvalIdx = 0;
    m_lastBeatT   = -1.0;
    m_beatInterval = 0.5;
}

void Visualizer::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    seed();
    updateTimerState();
}

void Visualizer::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_timer->stop();
}

void Visualizer::seed() {
    seedClouds();
    seedScenery();
    m_gulls.clear(); // gulls are spawned by the music, not seeded
}

void Visualizer::seedClouds() {
    const qreal w = qMax(1, width()), h = qMax(1, height());
    m_clouds.clear();
    for (int i = 0; i < kClouds; ++i) {
        Cloud c;
        c.x = frand(0, w);
        c.y = frand(h * 0.05, h * 0.45);
        c.scale = frand(0.7, 1.5);
        c.speed = frand(0.10, 0.40);
        // A varied cluster of overlapping puffs — sizes/offsets relative to the
        // widget height so clouds scale consistently across window sizes.
        const int puffs = 3 + QRandomGenerator::global()->bounded(4); // 3..6
        const qreal spread = h * 0.14;
        for (int j = 0; j < puffs; ++j) {
            c.puffs.push_back(QPointF(frand(-spread, spread), frand(-h * 0.04, h * 0.03)));
            c.pr.push_back(frand(h * 0.028, h * 0.06));
        }
        m_clouds.push_back(c);
    }
}

void Visualizer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    seedScenery(); // deterministic shore geometry, replayed at the new size (same scene, rescaled)

    // Rescale the clouds AND the living flock proportionally so nothing snaps size
    // or jumps position on a resize — their coords are absolute pixels from the old
    // dimensions. (The scenery is fraction-based, so seedScenery already rescales
    // it; only these moving, drifted bodies need the explicit scale.)
    const QSize old = event->oldSize();
    if (old.width() > 0 && old.height() > 0 && width() > 0 && height() > 0) {
        const qreal fx = qreal(width()) / old.width();
        const qreal fy = qreal(height()) / old.height();
        for (Cloud& c : m_clouds) {
            c.x *= fx; c.y *= fy;
            for (QPointF& pp : c.puffs) pp *= fy; // puff offsets/sizes are height-relative
            for (qreal& r : c.pr) r *= fy;
        }
        for (Gull& g : m_gulls) {
            g.x *= fx; g.y *= fy;
            g.size *= fy; g.speed *= fy;
            g.swoopAmp *= fy; g.vy *= fy;
        }
    } else {
        seedClouds(); // first sizing: no old geometry to rescale from
    }
}

void Visualizer::seedScenery() {
    const qreal w = qMax(1, width()), h = qMax(1, height());

    // Deterministic layout: a fixed per-session seed so every (re)build lays out the
    // SAME scene, only rescaled to the current size — a resize must not reshuffle the
    // hills/trees/rocks/stars. All positions are fractions of w/h, so replaying the
    // same rolls at a new size rescales cleanly. The star COUNT scales with area, so
    // stars draw from their OWN stream — otherwise a size-dependent star count would
    // shift the shore rng and reshuffle the trees/cliff/rocks on every resize.
    QRandomGenerator rng(m_sceneSeed);                       // shore geometry (size-independent draw count)
    QRandomGenerator rngStars(m_sceneSeed ^ 0x9E3779B9u);    // stars (variable count, independent)
    auto frand = [&](qreal lo, qreal hi) { return lo + (hi - lo) * rng.generateDouble(); };
    auto srand = [&](qreal lo, qreal hi) { return lo + (hi - lo) * rngStars.generateDouble(); };
    auto brand = [&](int n) { return int(rng.bounded(quint32(qMax(1, n)))); };

    // Night stars: scattered over the sky, denser up high, each with its own
    // twinkle phase. Seeded, so the constellation holds still frame to frame.
    m_stars.clear();
    const int nStars = int(qBound(50.0, w * h / 14000.0, 160.0));
    for (int i = 0; i < nStars; ++i) {
        Star s;
        s.x  = srand(0, w);
        s.y  = srand(0, h * 0.62) * srand(0.35, 1.0); // bias upward
        s.r  = srand(0.6, 1.7) * qMax(1.0, h / 640.0);
        s.tw = srand(0, 6.28);
        m_stars.push_back(s);
    }

    // The distant shoreline along the horizon: a gently rolling grass line above
    // a sliver of sand at the waterline. Sampled from smooth sines with random
    // phases so every launch rolls a little differently, and trees sit EXACTLY
    // on the sampled line.
    const qreal ph1 = frand(0, 6.28), ph2 = frand(0, 6.28);
    // A TALL hill: ridge up at ~0.60h, so even when the waves wet its lower
    // slice at max, the big green mass above stays dry — it can never read
    // as flooded.
    auto grassEdge = [&](qreal xn) {
        return h * (0.600 + 0.014 * std::sin(xn * 6.8 + ph1)
                          + 0.007 * std::sin(xn * 14.7 + ph2));
    };
    m_grass.clear();
    const int NG = 24;
    for (int i = 0; i <= NG; ++i) {
        const qreal xn = qreal(i) / NG;
        m_grass << QPointF(xn * w, grassEdge(xn));
    }
    m_grass << QPointF(w, h * 0.712) << QPointF(0, h * 0.712);

    // Little pines scattered along the grass line.
    m_trees.clear();
    const int nTrees = 8 + brand(5);
    for (int i = 0; i < nTrees; ++i) {
        Tree t;
        const qreal xn = frand(0.24, 0.97); // clear of the lighthouse point at far left
        t.x = xn * w;
        t.y = grassEdge(xn) + h * 0.002;
        t.s = frand(0.014, 0.030) * h;
        m_trees.push_back(t);
    }

    // The lighthouse point: a small rocky rise at the far-left shoreline, its
    // plateau just proud of the grass. Jitter is small enough that the tower
    // always has solid footing.
    auto jx = [&](qreal f) { return f * w + frand(-0.004, 0.004) * w; };
    auto jy = [&](qreal f) { return f * h + frand(-0.006, 0.006) * h; };
    m_cliff.clear();
    m_cliff << QPointF(-4, 0.745 * h)               // far-left, under the waterline
            << QPointF(-4, jy(0.548))               // a steep face rising straight out of the sea
            << QPointF(jx(0.020), jy(0.520))        // up to the headland's shoulder
            << QPointF(jx(0.056), jy(0.512))        // the plateau — sits WELL above the island ridge (~0.60h)
            << QPointF(jx(0.088), jy(0.534))        // dips to a saddle
            << QPointF(jx(0.116), jy(0.590))        // then a craggy face plunging back down
            << QPointF(jx(0.104), jy(0.646))
            << QPointF(jx(0.134), jy(0.708))
            << QPointF(0.142 * w, 0.745 * h);       // back into the sea
    m_lightBase = QPointF(0.038 * w, 0.516 * h);    // on the plateau top, clear of its edges

    // A couple of tiny rocks breaking the waterline off the point, settled a few
    // px lower so they sit IN the water rather than hovering at the sand line.
    m_rocks.clear();
    for (int i = 0; i < 2; ++i) {
        const qreal cx = w * (0.155 + 0.038 * i), cy = h * (0.714 + 0.007 * i) + 3.0;
        const qreal r  = h * frand(0.010, 0.017);
        QPolygonF rock;
        rock << QPointF(cx - r, cy + r * 0.5) << QPointF(cx - r * 0.55, cy - r * 0.7)
             << QPointF(cx + r * 0.15, cy - r) << QPointF(cx + r * 0.9, cy - r * 0.35)
             << QPointF(cx + r, cy + r * 0.5);
        m_rocks.push_back(rock);
    }
}

void Visualizer::recycleGull(Gull& g) {
    const qreal w = qMax(1, width()), h = qMax(1, height());
    const int dir = (m_behavior == GullBehavior::Reverse) ? -1 : 1; // Reverse flies R->L
    g.size  = frand(h * 0.015, h * 0.038);        // smaller = farther away (foreground kept
                                                  // modest so the tugboat still dwarfs them)
    g.x     = (dir > 0) ? -g.size : (w + g.size); // spawn at the trailing edge
    g.y     = frand(h * 0.08, h * 0.55);          // up in the sky, not close/low
    // Speed scales with size (near = faster) but is FLOORED: a small unlucky
    // roll used to crawl at ~10 px/s and read as stuck mid-air.
    g.speed = qMax(0.55, frand(0.4, 1.4) * (g.size / (h * 0.045)));
    g.phase = frand(0, 6.28);
    g.flap  = frand(0.8, 1.3);
    g.foff  = QRandomGenerator::global()->bounded(qMax(1, m_gullFrames.size()));
    g.yoff  = frand(-0.12, 0.12);                 // flock-band offset (fraction of height)
    g.rot = 0.0; g.spin = 0.0; g.vy = 0.0; g.dying = false;
    g.swoopP = -1.0; g.swoopAmp = 0.0; g.swoopDur = 1.3; // start flying level (swoops kick in at random)
}

void Visualizer::step() {
    const qreal dt = 1.0 / kFps;
    m_t += dt;

    // Cycle: the position poll only lands every 250ms, so glide the shown time of
    // day toward the target each frame instead of jumping on each poll. A big gap
    // (a seek) snaps; otherwise a ~0.8s first-order lag turns the stepped target
    // into a smooth, near-constant-velocity drift across the sky.
    if (m_cycle) {
        if (std::abs(m_todProgress - m_todShown) > 0.06) m_todShown = m_todProgress; // seek
        else m_todShown += (m_todProgress - m_todShown) * 0.02;
    }

    if (m_demo) {
        m_tLevel  = 0.30 + 0.30 * (0.5 + 0.5 * std::sin(m_t * 1.7))
                          + 0.15 * (0.5 + 0.5 * std::sin(m_t * 5.3));
        m_tBass   = 0.5 + 0.5 * std::sin(m_t * 1.3);
        m_tMid    = 0.5 + 0.5 * std::sin(m_t * 0.7 + 1.0);
        m_tTreble = 0.5 + 0.5 * std::sin(m_t * 2.1 + 2.0);
        m_demoBeatT += dt;
        if (m_demoBeatT >= 0.5) { m_demoBeatT = 0.0; beat(); }
    }

    // Responsive easing = low latency (the sky tracks the sound closely).
    // Snappy attack + smooth release, but gentle enough not to chase per-chunk
    // noise (that was the jitter).
    m_level  = easeAR(m_level,  m_tLevel,  0.30, 0.12);
    m_bass   = easeAR(m_bass,   m_tBass,   0.34, 0.13);
    m_mid    = easeAR(m_mid,    m_tMid,    0.30, 0.11);
    m_treble = easeAR(m_treble, m_tTreble, 0.50, 0.16); // snappier: hats are short transients
    // Lighthouse optic: spins at a default rate until the BPM is locked, then the
    // rate SLEWS toward the tempo target instead of jumping — a fresh lock or a
    // tempo change spins up/down smoothly, never a jarring speed change. beat()'s
    // phase pull is folded in over the same window. Flash cadence is the user's
    // choice: pi/m_beamBeats of plan rotation per beat = a flash every m_beamBeats.
    {
        const qreal k = qMin(1.0, dt * 2.5);
        m_beamRate  += (kPi / m_beamBeats / m_beatInterval - m_beamRate) * k;
        m_beamA     += dt * m_beamRate + m_beamNudge * k;
        m_beamNudge *= (1.0 - k);
    }

    // The water: ONE simulated system. Advect the surfaces rightward, then hold
    // the off-screen wave generator at each band's live level — the water
    // leaving it IS the dance, peeling off continuously with no seam.
    {
        // Water pace follows the BPM detector, slewed so a new tempo lock glides
        // the current in rather than snapping it.
        m_waveSpeed += (m_tempoSpeed - m_waveSpeed) * qMin(1.0, dt * 1.5);
        const qreal bands[3] = { m_treble, m_mid, m_bass };

        // Advect the surfaces: uniform speed (an amplitude-dispersion experiment
        // steepened crests into saw teeth — reverted), sampled with Catmull-Rom
        // (4-tap, monotone-clamped) so crests keep their shape across the whole
        // crossing — linear interp acted as a per-frame blur. NO decay: the
        // travelling waves don't rot away; distance shrinks only the ceiling of
        // their dance, applied at draw time. The cubic's wide stencil reads
        // downstream cells, so the update runs through a scratch row.
        for (int b = 0; b < 3; ++b) {
            const qreal shift = kWaveV[b] * qMin(m_waveSpeed, kWaveVMax) * dt * (kSurfN / kWaveSpan);
            qreal next[kSurfN];
            for (int i = 0; i < kSurfN; ++i) {
                const qreal src = i - shift;
                qreal v = 0.0;
                if (src > 0.0) {
                    const int   s1 = int(src);
                    const qreal f  = src - s1;
                    const int s0 = qMax(0, s1 - 1);
                    const int s2 = qMin(kSurfN - 1, s1 + 1);
                    const int s3 = qMin(kSurfN - 1, s1 + 2);
                    const qreal p0 = m_surf[b][s0], p1 = m_surf[b][s1];
                    const qreal p2 = m_surf[b][s2], p3 = m_surf[b][s3];
                    v = p1 + 0.5 * f * (p2 - p0 + f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3
                                                       + f * (3.0 * (p1 - p2) + p3 - p0)));
                    v = qBound(qMin(p1, p2), v, qMax(p1, p2)); // monotone: no ringing overshoot
                }
                next[i] = v;
            }
            std::memcpy(m_surf[b], next, sizeof(next));
            // The generator mound, held at the band's LIVE level (qMax: a fast
            // drop lets the taller water it already made keep rolling out).
            for (int i = 0; i < kSurfN; ++i) {
                const qreal xn = qreal(i) / (kSurfN - 1) * kWaveSpan - (kWaveSpan - 1.0);
                const qreal dd = std::abs(xn - kGenX) / kGenHw;
                if (dd >= 1.0) continue;
                const qreal prof = 0.5 * (1.0 + std::cos(dd * kPi));
                m_surf[b][i] = qMax(m_surf[b][i], prof * bands[b]);
            }
        }
    }

    // The tugboat: rare, one at a time, on a random sheet — and he ALTERNATES
    // direction, never making the same crossing twice in a row. His vertical
    // (buoyancy) physics run in drawShore where the water surface is known.
    if (m_tug.active) {
        m_tug.x   += m_tug.dir * dt * 0.055; // ~18s to cross the water
        m_tug.bob += dt * 2.1;
        if (m_tug.x < -0.10 || m_tug.x > 1.10) m_tug.active = false;
    } else if (frand(0.0, 1.0) < dt / 35.0) { // on average one visit every ~35s
        m_tug.active = true;
        m_tug.dir    = -m_tug.dir;
        m_tug.layer  = QRandomGenerator::global()->bounded(3);
        m_tug.x      = (m_tug.dir > 0) ? -0.05 : 1.05; // enter from the trailing side
        m_tug.bob    = frand(0, 6.28);
        m_tug.lastT  = -1.0; // settle onto the water on the first physics tick
        m_tug.vy     = 0.0;
        m_tug.tilt   = 0.0;
        m_tug.smokeT = 0.0;
    }
    // Age his smoke: the puffs live in world space, rising and drifting on a
    // light breeze — the trail hangs in the air and outlives his exit.
    for (int i = 0; i < m_smoke.size(); ++i) {
        Puff& pf = m_smoke[i];
        pf.age += dt;
        pf.y   -= height() * 0.045 * dt;
        pf.x   += pf.drift * height() * 0.030 * dt;
        if (pf.age > 1.6) { m_smoke.removeAt(i); --i; }
    }

    // Bottom-sky hue: spectral balance (0 bassy .. 1 trebly), eased SLOWLY so the
    // colour drifts gently rather than jumping with every transient.
    const qreal tot = m_bass + m_mid + m_treble + 1e-4;
    const qreal toneInst = qBound(0.0, (m_mid * 0.5 + m_treble) / tot, 1.0);
    m_colorTone = ease(m_colorTone, toneInst, 0.02);

    // Gif plays at its native speed (driven by the captured frame delays), NOT
    // the audio level.
    if (!m_gullFrames.isEmpty()) {
        m_frameAcc += dt * 1000.0;
        for (int guard = 0; guard < m_gullFrames.size() &&
                            m_frameAcc >= m_frameDelays[m_frameIdx]; ++guard) {
            m_frameAcc -= m_frameDelays[m_frameIdx];
            m_frameIdx = (m_frameIdx + 1) % m_gullFrames.size();
        }
    }

    const qreal w = width(), h = height();
    for (Cloud& c : m_clouds) {
        c.x += c.speed; // steady drift, not audio-tied
        if (c.x - c.scale * 140 > w) { c.x = -c.scale * 140; c.y = frand(h * 0.08, h * 0.5); }
    }

    // Flock size tracks loudness (capped). Only LIVING gulls count — ones falling
    // out at end-of-song don't block fresh spawns.
    int living = 0;
    for (const Gull& g : m_gulls) if (!g.dying) ++living;
    const int desired = m_dying ? 0 : int(std::lround(m_level * m_maxGulls));
    m_spawnT += dt;
    if (!m_dying && living < desired && m_spawnT > 0.12) {
        Gull g; recycleGull(g);
        m_gulls.push_back(g);
        m_spawnT = 0.0;
        ++living;
    }

    const int dir = (m_behavior == GullBehavior::Reverse) ? -1 : 1;
    const qreal flockY = h * 0.40 + std::sin(m_t * 0.6) * h * 0.16; // wandering flock band
    for (int i = 0; i < m_gulls.size(); ++i) {
        Gull& g = m_gulls[i];
        g.phase += 0.085 * g.flap; // bob clock (constant, not audio-tied)
        if (g.dying) {
            g.vy  += 0.0009 * h;  // gravity
            g.y   += g.vy;
            g.x   += g.speed * 0.3;
            g.rot += g.spin;
            if (g.y - g.size > h) { m_gulls.removeAt(i); --i; } // fell off the bottom
            continue;
        }
        // Glide (direction per behaviour, BPM-paced: the tempo detector is the
        // main speed input, with a floor that keeps even slow songs' gulls
        // visibly underway). A swooping gull picks up speed as it drops and
        // bleeds it off climbing out — fastest at the bottom.
        const qreal surge = swoopSurge(g.swoopP); // 1.0 when not swooping; fast dive, carried glide up
        g.x += dir * g.speed * (0.9 + 1.1 * m_tempoSpeed) * surge;
        if (m_behavior == GullBehavior::Flocking && g.swoopP < 0.0) // cohere toward the wandering
            g.y += (flockY + g.yoff * h - g.y) * 0.03;              // flock band (not mid-swoop)
        // An active swoop always runs to completion — even if the behaviour was
        // switched away mid-arc, the bird finishes its dive cleanly. Only STARTING
        // a new dive requires Swooping. Long durations + the dive surge make the
        // arc WIDE — a gliding banked swoop across the sky, not a vertical dip.
        if (g.swoopP >= 0.0) {
            g.swoopP += dt / g.swoopDur;
            if (g.swoopP >= 1.0) g.swoopP = -1.0;
        } else if (m_behavior == GullBehavior::Swooping && frand(0.0, 1.0) < 0.005) {
            g.swoopP   = 0.0;
            g.swoopAmp = frand(0.16, 0.32) * h; // wider, deeper arc
            g.swoopDur = frand(2.2, 3.0); // seconds; varied so the flock doesn't sync up
        }
        const bool off = (dir > 0) ? (g.x - g.size > w) : (g.x + g.size < 0);
        if (off) {
            if (living > desired) { m_gulls.removeAt(i); --i; --living; } // thin to target
            else recycleGull(g);
        }
    }
    update();
}

void Visualizer::drawGull(QPainter& p, const Gull& g) {
    if (m_gullFrames.isEmpty()) return; // animated gulls only now

    const int dir = (m_behavior == GullBehavior::Reverse) ? -1 : 1;
    const bool swooping = !g.dying && g.swoopP >= 0.0; // by state, not behaviour: a switch
                                                       // mid-arc still finishes the dive

    // Vertical motion per behaviour: a dive arc for Swooping, a gentle bob
    // otherwise (frozen while tumbling at EOF). A swooping gull also pitches its
    // nose along the flight path — steep down into the dive, up on the climb.
    qreal vOff, tilt = 0.0;
    if (g.dying) {
        vOff = 0.0;
    } else if (swooping) {
        qreal slope;
        const qreal depth = swoopDepth(g.swoopP, &slope);
        // Bob amplitude blends with depth: full 0.20 at the swoop's ends (exactly
        // matching level flight, so entry/exit are seamless), nearly gone at the
        // bottom of the arc where the gull is committed to the glide.
        vOff = depth * g.swoopAmp + std::sin(g.phase) * g.size * (0.20 - 0.14 * depth);
        // Pitch = the true path tangent (vertical vs horizontal velocity, px/s),
        // relaxed a touch and capped so the bank reads natural, never aerobatic.
        // (Horizontal term mirrors step()'s BPM-paced glide + dive surge.)
        const qreal vx = g.speed * (0.9 + 1.1 * m_tempoSpeed) * swoopSurge(g.swoopP) * 60.0;
        const qreal vy = slope * g.swoopAmp / g.swoopDur;
        tilt = qBound(-50.0, std::atan2(vy, qMax(1.0, vx)) * (180.0 / 3.14159265358979) * 0.75, 50.0) * dir;
    } else if (m_behavior == GullBehavior::Flocking) {
        vOff = std::sin(g.phase) * g.size * 0.10;
    } else {
        vOff = std::sin(g.phase) * g.size * 0.20;
    }

    // Wings through a swoop: frame 0 (wings tucked) is HELD on the way down; the
    // open (0 -> most-outstretched glide) plays FORWARD only and is timed to FINISH
    // right at the bottom of the arc — it starts a little early during the dive's
    // tail so the glide is held from the bottom, not a beat after it. That spread
    // is then HELD through the climb. Normal flapping resumes once the swoop ends.
    int fi = (m_frameIdx + g.foff) % m_gullFrames.size();
    if (swooping) {
        const int glide = qBound(0, kGlideHoldFrame, int(m_gullFrames.size()) - 1);
        const qreal finish = kSwoopBottom + kFlapLate; // the open lands here (a hair past the arc's bottom)
        if (g.swoopP >= finish) {
            fi = glide; // glide held from there onward
        } else {
            // Slowed time to open 0 -> glide, expressed in swoopP, so the open ENDS
            // at `finish` (it begins that far before it). kFlapSlow just grows the
            // lead, so the finish stays pinned in place.
            qreal openMs = 0.0;
            for (int k = 0; k < glide; ++k) openMs += m_frameDelays[k] * kFlapSlow;
            // Never longer than the descent to `finish` (else it couldn't land on
            // time): cap it so a slow flap just fills the descent, keeping a sliver
            // of tuck at the top.
            openMs = qMin(openMs, finish * g.swoopDur * 1000.0 * kFlapMaxDive);
            const qreal leadP = (g.swoopDur > 0.0) ? (openMs / 1000.0) / g.swoopDur : 0.0;
            const qreal openStart = finish - leadP;
            if (g.swoopP < openStart) {
                fi = 0; // dive: wings tucked, held
            } else {
                qreal tms = (g.swoopP - openStart) * g.swoopDur * 1000.0;
                int f = 0; qreal acc = 0.0;
                while (f < glide && tms >= acc + m_frameDelays[f] * kFlapSlow) { acc += m_frameDelays[f] * kFlapSlow; ++f; }
                fi = f;
            }
        }
    }
    const QPixmap& fr = m_gullFrames[fi];
    const qreal tw = g.size * 2.6; // wingspan
    const qreal th = tw * fr.height() / fr.width();

    p.save();
    p.translate(g.x, g.y + vOff);
    if (g.rot != 0.0 || tilt != 0.0) p.rotate(g.rot + tilt);
    if (dir > 0) p.scale(-1.0, 1.0); // frames face left natively; flip to face travel direction
    const QRectF dst(-tw / 2, -th / 2, tw, th);
    const qreal na = (m_cycleNight >= 0.0) ? m_cycleNight : (m_mode == Mode::Night ? 1.0 : 0.0);
    if (na > 0.001 && fi < m_gullFramesDark.size()) {
        // As night falls the gulls darken (dark frame fades in over the lit one),
        // and light back up wherever the lighthouse beam catches them. At na = 1
        // the dark frame is fully opaque — identical to the fixed Night scene.
        p.drawPixmap(dst, fr, fr.rect());
        p.setOpacity(na);
        p.drawPixmap(dst, m_gullFramesDark[fi], fr.rect());
        p.setOpacity(1.0);
        const qreal lit = beamLightAt(g.x, g.y + vOff);
        if (lit > 0.01) {
            p.setOpacity(lit);
            p.drawPixmap(dst, fr, fr.rect());
            p.setOpacity(1.0);
        }
    } else {
        p.drawPixmap(dst, fr, fr.rect());
    }
    p.restore();
}

void Visualizer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_cycle) { renderCycle(p, m_todShown); return; }

    m_cycleNight = -1.0; // fixed scene: helpers use the discrete m_mode, not a ramp
    if (paletteFor(m_mode).reactiveSky) drawSky(p);   // Morning/Dusk: reactive per-band EQ sky
    else                                drawWaves(p); // Day/Night: static day / starry night

    drawClouds(p);
    // Draw smallest-first so bigger (nearer) gulls paint OVER smaller (farther)
    // ones — fakes perspective depth.
    QVector<const Gull*> order;
    order.reserve(m_gulls.size());
    for (const Gull& g : m_gulls) order.push_back(&g);
    std::sort(order.begin(), order.end(),
              [](const Gull* a, const Gull* b) { return a->size < b->size; });
    for (const Gull* g : order) drawGull(p, *g);
}

// ===== Seagull Cycle: a full day driven by song progress =====================

QColor Visualizer::lerpColor(const QColor& a, const QColor& b, qreal t) {
    t = qBound(0.0, t, 1.0);
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

// Which two keyframes bound this instant, and how far between them (eased). The
// final band blends Night BACK to Morning, so at tod=1 the sky equals tod=0.
Visualizer::Seg Visualizer::segmentFor(qreal tod) const {
    if (tod < kMornHoldEnd)    return { Mode::Morning, Mode::Morning, 0.0 };
    if (tod < kDayHoldStart)   return { Mode::Morning, Mode::Day,   smooth01((tod - kMornHoldEnd)   / (kDayHoldStart   - kMornHoldEnd)) };
    if (tod < kDayHoldEnd)     return { Mode::Day,     Mode::Day,    0.0 };
    if (tod < kDuskHoldStart)  return { Mode::Day,     Mode::Dusk,  smooth01((tod - kDayHoldEnd)    / (kDuskHoldStart  - kDayHoldEnd)) };
    if (tod < kDuskHoldEnd)    return { Mode::Dusk,    Mode::Dusk,   0.0 };
    if (tod < kNightHoldStart) return { Mode::Dusk,    Mode::Night, smooth01((tod - kDuskHoldEnd)   / (kNightHoldStart - kDuskHoldEnd)) };
    if (tod < kNightHoldEnd)   return { Mode::Night,   Mode::Night,  0.0 };
    return { Mode::Night, Mode::Morning, smooth01((tod - kNightHoldEnd) / (1.0 - kNightHoldEnd)) }; // dawn returns
}

// 0 by day, up across dusk, full at night, easing back to 0 by dawn (tod=1) so
// the loop closes. Drives stars, the beam, the gulls' dark shading, cloud dim.
qreal Visualizer::nightness(qreal tod) const {
    if (tod < kNightRiseStart) return 0.0;
    if (tod < kNightRiseEnd)   return smooth01((tod - kNightRiseStart) / (kNightRiseEnd - kNightRiseStart));
    if (tod < kNightFallStart) return 1.0;
    return 1.0 - smooth01((tod - kNightFallStart) / (1.0 - kNightFallStart)); // -> 0 at tod=1
}

// Music-reactive warmth: peaks at Morning (tod 0/1) and Dusk (0.5), eases to a
// low floor at Day (0.25) and Night (0.75). A cosine keeps it smooth AND periodic,
// so it matches at the loop seam.
qreal Visualizer::reactiveAmt(qreal tod) const {
    const qreal bump = 0.5 + 0.5 * std::cos(4.0 * kPi * tod);
    return kReactFloor + (1.0 - kReactFloor) * bump;
}

// The five vertical sky stops for a keyframe. Reactive scenes hand back their
// skyEq* stops; static scenes (Day/Night) synthesise five from their two, so the
// one reactive-column renderer can draw them as a plain smooth gradient.
std::array<QColor, 5> Visualizer::skyStopsFor(Mode m) const {
    const ScenePalette s = paletteFor(m);
    if (s.reactiveSky)
        return { s.skyEqTop, s.skyEqHigh, s.skyEqRose, s.skyEqAmb, s.skyEqGold };
    return { s.skyTop,
             lerpColor(s.skyTop, s.skyBot, 0.35),
             lerpColor(s.skyTop, s.skyBot, 0.60),
             lerpColor(s.skyTop, s.skyBot, 0.82),
             s.skyBot };
}

// Shore/water colours lerped between the two bounding keyframes (crestShimmer is
// pulled straight from Night and gated by nightness in drawShore, so it isn't
// needed here).
Visualizer::ScenePalette Visualizer::todPalette(qreal tod) const {
    const Seg s = segmentFor(tod);
    const ScenePalette a = paletteFor(s.a), b = paletteFor(s.b);
    ScenePalette r;
    r.grass       = lerpColor(a.grass,      b.grass,      s.f);
    r.sand        = lerpColor(a.sand,       b.sand,       s.f);
    r.trees       = lerpColor(a.trees,      b.trees,      s.f);
    r.sandShadowA = int(lerp(a.sandShadowA, b.sandShadowA, s.f));
    r.waterBack   = lerpColor(a.waterBack,  b.waterBack,  s.f);
    r.waterMid    = lerpColor(a.waterMid,   b.waterMid,   s.f);
    r.waterFront  = lerpColor(a.waterFront, b.waterFront, s.f);
    return r;
}

// The unified sky: the same per-band EQ-column machinery as drawSky, but built
// from the blended stops and with the warm "bleed" scaled by reactiveAmt so it
// pumps at sunrise/sunset and settles flat through midday and night. No veiled
// sun here — Cycle draws its travelling sun over the top.
void Visualizer::drawCycleSky(QPainter& p, qreal tod) {
    const qreal w = width(), h = height();
    const Seg s = segmentFor(tod);
    const std::array<QColor, 5> sa = skyStopsFor(s.a), sb = skyStopsFor(s.b);
    QColor c[5];
    for (int i = 0; i < 5; ++i) c[i] = lerpColor(sa[i], sb[i], s.f);

    const qreal ra = reactiveAmt(tod);
    const qreal e  = qBound(0.0, m_level, 1.0) * ra; // saturation pump scales with reactivity

    auto glow = [e](const QColor& base) {
        float hh, ss, l, a; base.getHslF(&hh, &ss, &l, &a);
        ss = float(qBound(0.0, double(ss) * (0.72 + 0.38 * e), 1.0));
        l  = float(qBound(0.0, double(l)  * (0.92 + 0.08 * e), 0.85));
        return QColor::fromHslF(hh, ss, l);
    };
    const QColor cTop  = glow(c[0]);
    const QColor cHigh = glow(c[1]);
    const QColor cRose = glow(c[2]);
    const QColor cAmb  = glow(c[3]);
    const QColor cGold = glow(c[4]);

    auto bandFrac = [&](qreal xn) {
        qreal a, b, t;
        if (xn < 0.5) { a = m_bass; b = m_mid;    t = xn / 0.5; }
        else          { a = m_mid;  b = m_treble; t = (xn - 0.5) / 0.5; }
        t = t * t * (3.0 - 2.0 * t);
        return qBound(0.0, a + (b - a) * t, 1.0);
    };

    p.setRenderHint(QPainter::Antialiasing, false);
    const int N = 80;
    const qreal colStep = w / N;
    for (int i = 0; i < N; ++i) {
        const qreal xn = (i + 0.5) / N;
        // The band only pushes the warm boundary up by ra: at the reactive floor
        // it sits low and steady (plain gradient), at full it climbs per band.
        const qreal warmTop = qBound(0.18, 1.0 - (0.12 + 0.72 * ra * bandFrac(xn)), 0.90);
        QLinearGradient g(0, 0, 0, h * 0.72);
        g.setColorAt(0.0, cTop);
        g.setColorAt(qBound(0.0, warmTop * 0.6, 1.0), cHigh);
        g.setColorAt(warmTop, cRose);
        g.setColorAt(qBound(0.0, warmTop + (1.0 - warmTop) * 0.5, 1.0), cAmb);
        g.setColorAt(1.0, cGold);
        p.fillRect(QRectF(i * colStep, 0, colStep + 1.0, h), g);
    }
    p.setRenderHint(QPainter::Antialiasing, true);
}

void Visualizer::renderCycle(QPainter& p, qreal tod) {
    m_cycleNight = nightness(tod); // shared helpers read this instead of m_mode
    const qreal w = width(), h = height();

    drawCycleSky(p, tod);

    // Stars fade in with nightness, lifted a touch by the treble (as at Night).
    if (m_cycleNight > 0.001) {
        p.setPen(Qt::NoPen);
        for (const Star& s : m_stars) {
            const qreal tw = 0.5 + 0.5 * std::sin(m_t * 1.6 + s.tw);
            const int a = int((60 + 120 * tw * (0.55 + 0.45 * m_treble)) * m_cycleNight);
            if (a <= 0) continue;
            p.setBrush(QColor(225, 235, 255, a));
            p.drawEllipse(QPointF(s.x, s.y), s.r, s.r);
        }
    }

    // Sun and moon share ONE east->west arc, half a day apart, so the loop closes:
    // the sun is up through the day half, the moon through the night half, and both
    // are periodic in tod (position at tod=1 == tod=0). The moon keeps moving all
    // night and sets in the west by dawn, rather than parking at a perch.
    const qreal sinceBeat = m_t - m_lastBeatT;
    const qreal beatPulse = (m_lastBeatT >= 0.0) ? std::exp(-sinceBeat * 6.5) : 0.0; // same pulse the sun uses
    auto arcBody = [&](qreal ph, qreal& outAlpha) -> QPointF {
        // ph in [0,1): rises at the east horizon (0), zenith at 0.25, sets off the
        // right edge (0.5), then below the horizon (0.5..1) -> not drawn.
        if (ph >= 0.5) { outAlpha = 0.0; return QPointF(); }
        const qreal a = ph / 0.5;                              // 0..1 across the visible sky
        const qreal x = lerp(0.06, 1.16, a) * w;               // east horizon -> off the right edge
        const qreal y = (0.74 - 0.60 * std::sin(a * kPi)) * h; // parabola: 0.74h horizon -> 0.14h zenith
        const qreal edge = 0.12;                               // fade in/out at the horizon
        outAlpha = qBound(0.0, (a < edge) ? a / edge : (a > 1.0 - edge) ? (1.0 - a) / edge : 1.0, 1.0);
        return QPointF(x, y);
    };

    // Moon first (behind the sun during the brief dusk/dawn overlap), pulsing on
    // the beat exactly like the sun.
    qreal moonA;
    const QPointF moonC = arcBody(std::fmod(tod + 0.5, 1.0), moonA);
    if (moonA > 0.001) {
        const qreal mr = h * 0.042 * (1.0 + 0.17 * beatPulse);
        p.setOpacity(moonA);
        QRadialGradient halo(moonC, mr * 3.4);
        halo.setColorAt(0.0, QColor(215, 226, 242, 80));
        halo.setColorAt(1.0, QColor(215, 226, 242, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo); p.drawEllipse(moonC, mr * 3.4, mr * 3.4);
        p.setBrush(QColor("#e6ebf3")); p.drawEllipse(moonC, mr, mr);
        p.setOpacity(1.0);
    }

    // The sun over the top, same arc half a day earlier (drawSun handles its pulse).
    qreal sunA;
    const QPointF sunC = arcBody(tod, sunA);
    if (sunA > 0.001) {
        p.setOpacity(sunA);
        drawSun(p, sunC, h * 0.075);
        p.setOpacity(1.0);
    }

    drawShore(p, todPalette(tod)); // waves + shore, night-gated bits via m_cycleNight
    drawClouds(p);

    QVector<const Gull*> order;
    order.reserve(m_gulls.size());
    for (const Gull& g : m_gulls) order.push_back(&g);
    std::sort(order.begin(), order.end(),
              [](const Gull* a, const Gull* b) { return a->size < b->size; });
    for (const Gull* g : order) drawGull(p, *g);
}

void Visualizer::drawSky(QPainter& p) {
    const ScenePalette pal = paletteFor(m_mode); // Morning (sunrise) or Dusk (sunset) stops + shore
    const qreal w = width(), h = height();
    const qreal e = qBound(0.0, m_level, 1.0);

    // Slight colour shift with the music (saturation only — never washes white).
    auto glow = [e](const QColor& base) {
        float hh, s, l, a; base.getHslF(&hh, &s, &l, &a);
        s = float(qBound(0.0, double(s) * (0.72 + 0.38 * e), 1.0));
        l = float(qBound(0.0, double(l) * (0.92 + 0.08 * e), 0.85));
        return QColor::fromHslF(hh, s, l);
    };
    const QColor cTop  = glow(pal.skyEqTop);  // zenith (deep indigo / violet)
    const QColor cHigh = glow(pal.skyEqHigh); // high sky (violet)
    const QColor cRose = glow(pal.skyEqRose); // the warm/cool bleed boundary (pink / rose)
    const QColor cAmb  = glow(pal.skyEqAmb);  // low warmth (orange)
    const QColor cGold = glow(pal.skyEqGold); // horizon (gold / amber)

    // The orange/pink bleeds UP into the indigo in the shape of a visual EQ: per
    // column, the warm/cool boundary sits at the frequency band for that x (bass
    // left -> treble right). Each column is a FULL vertical sunrise gradient with
    // its boundary moved, so it stays one smooth sky (no lines, no flat overlay)
    // whose warm silhouette pumps per-band — it never "tops out" because every
    // band moves independently.
    auto bandFrac = [&](qreal xn) {
        qreal a, b, t;
        if (xn < 0.5) { a = m_bass; b = m_mid;    t = xn / 0.5; }
        else          { a = m_mid;  b = m_treble; t = (xn - 0.5) / 0.5; }
        t = t * t * (3.0 - 2.0 * t);
        return qBound(0.0, a + (b - a) * t, 1.0);
    };

    // The gradient spans only down to the waterline — the gold horizon lands where
    // sea meets sky, and the shore + waves own everything below (its final stop
    // pads the covered remainder).
    //
    // Morning: the sun sits BEHIND this glow. Paint an opaque warm base and the
    // sun onto it first, then lay the reactive sky OVER them on an offscreen layer
    // with a soft transparency pool punched around the sun (DestinationOut). The
    // light stays only SLIGHTLY see-through at the sun, so the sun is veiled and
    // TINTED by the reactive glow — its bright silhouette bleeds through as the
    // source of the light rather than a disc sitting in front of it.
    const bool hasSun = (m_mode == Mode::Morning);
    const QPointF sunC(w * 0.5, h * 0.600); // higher, so the bright core clears the hill (not just a rim)
    const qreal   sunR = h * 0.088;
    if (hasSun) {
        p.fillRect(rect(), cGold);        // opaque warm base: the thinned pool never bares the widget
        drawSun(p, sunC, sunR);
    }

    // Build the sky. With a sun behind it, render onto an offscreen layer so its
    // alpha can be thinned around the sun before compositing; otherwise paint
    // straight to the widget.
    const qreal dpr = devicePixelRatioF();
    QImage layer;
    QPainter ip;
    QPainter* sp = &p;
    if (hasSun) {
        layer = QImage(QSize(qMax(1, int(w * dpr)), qMax(1, int(h * dpr))),
                       QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        ip.begin(&layer);
        sp = &ip;
    }
    sp->setRenderHint(QPainter::Antialiasing, false);
    const int N = 80;
    const qreal colStep = w / N;
    for (int i = 0; i < N; ++i) {
        const qreal xn = (i + 0.5) / N;
        // Boundary position from the top (0..1): a little bleed even when silent,
        // most of the sky when that band is loud.
        const qreal warmTop = qBound(0.18, 1.0 - (0.12 + 0.72 * bandFrac(xn)), 0.86);
        QLinearGradient g(0, 0, 0, h * 0.72);
        g.setColorAt(0.0, cTop);
        g.setColorAt(qBound(0.0, warmTop * 0.6, 1.0), cHigh);
        g.setColorAt(warmTop, cRose);
        g.setColorAt(qBound(0.0, warmTop + (1.0 - warmTop) * 0.5, 1.0), cAmb);
        g.setColorAt(1.0, cGold);
        sp->fillRect(QRectF(i * colStep, 0, colStep + 1.0, h), g);
    }
    if (hasSun) {
        // Thin the sky over the sun: DestinationOut subtracts alpha, most at the
        // centre (sun shines through), easing back to fully-opaque sky at the pool
        // edge. Keep the centre alpha WELL under 255 so the light only goes
        // slightly transparent — the sun stays veiled, not cut out.
        ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        QRadialGradient hole(sunC, sunR * 1.5);
        hole.setColorAt(0.00, QColor(0, 0, 0, 255)); // core: sky fully removed -> sun at full brightness
        hole.setColorAt(0.45, QColor(0, 0, 0, 175)); // then the sky closes back in...
        hole.setColorAt(0.78, QColor(0, 0, 0,  70));
        hole.setColorAt(1.00, QColor(0, 0, 0,   0)); // ...to fully opaque, so the rim melts into the glow
        ip.fillRect(QRectF(0, 0, w, h), hole);
        ip.end();
        p.drawImage(0, 0, layer);
    }
    p.setRenderHint(QPainter::Antialiasing, true);

    drawShore(p, pal); // the coast under this sky (day-lit for Morning, dusk-lit for Dusk)
}

void Visualizer::drawSun(QPainter& p, const QPointF& c, qreal r) {
    // A solid gold sun. It holds its place — no bob, no idle breathing — and its
    // SIZE punches out on every beat, then eases back before the next. beat = 1 at
    // the hit, decaying to 0 (from the tempo clock's last-beat time, so it works on
    // the synthesised demo beats too). Morning draws it BEHIND the reactive sky,
    // which then veils and tints it; Day draws it straight onto the blue sky.
    const qreal sinceBeat = m_t - m_lastBeatT;
    const qreal beat = (m_lastBeatT >= 0.0) ? std::exp(-sinceBeat * 6.5) : 0.0;
    const qreal rp   = r * (1.0 + 0.17 * beat); // beat-pulsed radius

    p.save();
    p.setPen(Qt::NoPen);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Rays: a fixed ring of little gold triangles around the disc, evenly spaced
    // and pointing straight outward, with a small gap between the circle and the
    // triangle bases (cartoon spacing). They don't spin — each just lengthens on
    // the beat and eases back.
    const int   rayCount = 12;
    const qreal baseR    = rp * 1.05;                  // base sits just off the rim -> a small gap to the circle
    const qreal tipR     = rp * (1.34 + 0.20 * beat);  // tip pushes out on the beat
    const qreal halfW    = (kPi / rayCount) * 0.42;    // angular half-width of each base
    p.setBrush(QColor(255, 188, 76));
    for (int i = 0; i < rayCount; ++i) {
        const qreal a = (2.0 * kPi * i) / rayCount;
        QPainterPath tri;
        tri.moveTo(c.x() + std::cos(a - halfW) * baseR, c.y() + std::sin(a - halfW) * baseR);
        tri.lineTo(c.x() + std::cos(a + halfW) * baseR, c.y() + std::sin(a + halfW) * baseR);
        tri.lineTo(c.x() + std::cos(a) * tipR,          c.y() + std::sin(a) * tipR);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // The disc: a clean PERFECT CIRCLE — warm gold with a near-white core so it's
    // the brightest point, and a crisp gold edge (no soft outer ring / halo).
    QRadialGradient disc(c, rp);
    disc.setColorAt(0.00, QColor(255, 251, 230)); // bright warm-white core
    disc.setColorAt(0.55, QColor(255, 233, 160));
    disc.setColorAt(1.00, QColor(255, 205, 104)); // solid gold rim, opaque = crisp edge
    p.setBrush(disc);
    p.drawEllipse(c, rp, rp);

    p.restore();
}

void Visualizer::drawWaves(QPainter& p) {
    const ScenePalette pal = paletteFor(m_mode);
    const bool night = pal.night;
    const qreal w = width(), h = height();

    // Sea sky: a time-of-day gradient — three stops for dusk's sunset band, two
    // for the plainer day and night skies.
    QLinearGradient sky(0, 0, 0, h);
    sky.setColorAt(0.0, pal.skyTop);
    if (pal.skyMid.isValid()) sky.setColorAt(0.55, pal.skyMid);
    sky.setColorAt(1.0, pal.skyBot);
    p.fillRect(rect(), sky);

    if (night) {
        // Stars twinkle on their own clocks, lifted a little by the treble; the
        // moon gets a soft halo.
        p.setPen(Qt::NoPen);
        for (const Star& s : m_stars) {
            const qreal tw = 0.5 + 0.5 * std::sin(m_t * 1.6 + s.tw);
            p.setBrush(QColor(225, 235, 255, int(60 + 120 * tw * (0.55 + 0.45 * m_treble))));
            p.drawEllipse(QPointF(s.x, s.y), s.r, s.r);
        }
        const QPointF moon(w * 0.80, h * 0.15);
        const qreal mr = h * 0.042;
        QRadialGradient halo(moon, mr * 3.4);
        halo.setColorAt(0.0, QColor(215, 226, 242, 80));
        halo.setColorAt(1.0, QColor(215, 226, 242, 0));
        p.setBrush(halo); p.drawEllipse(moon, mr * 3.4, mr * 3.4);
        p.setBrush(QColor("#e6ebf3")); p.drawEllipse(moon, mr, mr);
    }

    // Day gets the same animated sun, hung high in the top-right sky — straight
    // onto the blue sky (no veil; Day's static sky doesn't glow on its own).
    if (m_mode == Mode::Day) drawSun(p, QPointF(w * 0.84, h * 0.17), h * 0.072);

    drawShore(p, pal);
}

void Visualizer::drawShore(QPainter& p, const ScenePalette& pal) {
    // Nightness for the night-gated bits (lighthouse beam, tug lamp, moonlit
    // crest): a continuous 0..1 while cycling, else the fixed scene's on/off.
    const qreal na = (m_cycleNight >= 0.0) ? m_cycleNight : (pal.night ? 1.0 : 0.0);
    const qreal w = width(), h = height();

    // The distant shoreline, all of it AT the horizon behind the water: rolling
    // grass, then a just-visible sliver of sand at the waterline (calm — no wave
    // motion touches it), little pines on the grass line, and the lighthouse
    // point far off on the left. The waves are drawn after, in front.
    p.setPen(Qt::NoPen);
    p.setBrush(pal.grass);
    p.drawPolygon(m_grass);
    // The sand line, with the land's soft shadow falling on its top edge — that
    // shadow is what separates it from the similar-toned horizon sky (Morning's
    // gold sits right on this boundary).
    const QRectF sandR(0, h * 0.700, w, h * 0.028);
    p.fillRect(sandR, pal.sand);
    QLinearGradient sandShadow(0, sandR.top(), 0, sandR.bottom());
    sandShadow.setColorAt(0.0,  QColor(40, 45, 60, pal.sandShadowA));
    sandShadow.setColorAt(0.80, QColor(40, 45, 60, 0));
    p.fillRect(sandR, sandShadow);
    p.setBrush(pal.trees);
    for (const Tree& t : m_trees) {
        // One solid triangle per pine — the old stacked pair overlapped and
        // Qt's odd-even fill hollowed the overlap out.
        QPainterPath pine;
        pine.moveTo(t.x - t.s * 0.40, t.y);
        pine.lineTo(t.x + t.s * 0.40, t.y);
        pine.lineTo(t.x, t.y - t.s);
        pine.closeSubpath();
        p.drawPath(pine);
    }
    drawLighthouse(p, na);

    // ONE water system: every sheet is its advected surface (step()), born at
    // the off-screen generator already dancing, rolling right and dancing as it
    // cruises — the recorded train is modulated by the band's LIVE level, and
    // distance shrinks only the CEILING of that dance (a fixed falloff), never
    // the wave itself. Quiet or steady => flat calm sea.
    //   front  (drawn last,  lowest,  lightest) = BASS (tallest)
    //   middle                                  = MIDS
    //   back   (drawn first, highest, darkest)  = TREBLE
    const qreal maxH = h * 0.20;

    // gain pushes a sheet harder into its headroom: the mid/back sheets have
    // far less room than bass (their rest lines sit near the ceiling), so
    // without extra drive they barely seemed to move.
    struct Layer { qreal baseFrac, band, gain, ampMul; QColor col; };
    const Layer layers[3] = {
        { 0.72, m_treble, 1.45, 1.0, pal.waterBack  }, // back   = treble
        { 0.79, m_mid,    1.30, 1.0, pal.waterMid   }, // middle = mids
        { 0.86, m_bass,   1.00, 2.3, pal.waterFront }, // front  = bass (taller)
    };
    p.setPen(Qt::NoPen);
    const int N = 80;
    for (int li = 0; li < 3; ++li) {
        const Layer& ly = layers[li];
        // Crest ceilings: the front and mid sheets may climb to halfway up the
        // hill (sand line ~0.700h to the treetops ~0.635h -> 0.667h); the back
        // sheet stays a low shimmer at mid-grass (0.682h).
        const qreal ceilY = (li == 0) ? h * 0.682 : h * 0.667;
        const qreal maxSwell = h * ly.baseFrac - ceilY;         // headroom to the ceiling
        const qreal drive = 1.6 * ly.gain / (ly.ampMul * maxH); // full swell ~= 92% of ceiling
        // The full surface-to-swell formula for this sheet, reused for both the
        // outline points and anything riding the water (the tugboat).
        auto swellAt = [&](qreal xn) -> qreal {
            const qreal pos = (xn + (kWaveSpan - 1.0)) / kWaveSpan * (kSurfN - 1);
            const int   c0  = qBound(0, int(pos), kSurfN - 2);
            const qreal f   = pos - c0;
            const qreal hgt = m_surf[li][c0] * (1.0 - f) + m_surf[li][c0 + 1] * f;
            const qreal distCap = 1.0 - 0.62 * xn; // max dance height falls off rightward
            // Smooth alternation between dancing water (rides the live band) and
            // steady cruising water, staggered per layer (kDancePhase).
            const qreal danceMask = 0.5 + 0.5 * std::sin(xn * kDanceFreq + kDancePhase[li]);
            const qreal lift = 0.55 + danceMask * (0.65 * ly.band - 0.20);
            const qreal swell = hgt * lift * distCap * ly.ampMul * maxH;
            // Soft limiter, not a clip: sized so a full pulse lands at ~92% of
            // the headroom — crests round off at the ceiling, never flat-top.
            return maxSwell * std::tanh(swell * drive);
        };
        QPainterPath wave, crest;
        wave.moveTo(0, h);
        for (int i = 0; i <= N; ++i) {
            const qreal xn = qreal(i) / N;
            const QPointF pt(xn * w, h * ly.baseFrac - swellAt(xn));
            wave.lineTo(pt);
            if (i == 0) crest.moveTo(pt); else crest.lineTo(pt);
        }
        wave.lineTo(w, h);
        wave.closeSubpath();
        p.fillPath(wave, ly.col);
        if (na > 0.001) { // night only: moonlight glinting off the crests, fading in with nightness
            // The day-lit scenes leave the water matte (no crest outline); only
            // night draws this line, brightening as the band swells.
            // Brush OFF: drawPath also FILLS an open path (closing it with a
            // straight chord), and a leftover fill brush would paint that chord
            // sliver as a strip across the wave.
            p.setBrush(Qt::NoBrush);
            QColor glint = paletteFor(Mode::Night).crestShimmer; // cool moonlight tone (blended pal has none)
            glint.setAlpha(int((28 + 70 * ly.band) * na));
            p.setPen(QPen(glint, qMax(1.0, h / 700.0)));
            p.drawPath(crest);
            p.setPen(Qt::NoPen);
        }
        if ((m_tug.active || !m_smoke.isEmpty()) && m_tug.layer == li) {
            const qreal s = h * ((li == 0) ? 0.050 : (li == 1) ? 0.070 : 0.098); // depth scale — a boat dwarfs a gull
            if (m_tug.active) {
                const qreal waterY = h * ly.baseFrac - swellAt(m_tug.x)
                                   + std::sin(m_tug.bob) * s * 0.06;
                // Physics once per animation tick (paint can fire more often).
                if (m_t != m_tug.lastT) {
                    const qreal pdt = 1.0 / kFps;
                    if (m_tug.lastT < 0.0) { m_tug.py = waterY; m_tug.vy = 0.0; }
                    const bool airborne = m_tug.py < waterY - 0.5;
                    if (airborne) {
                        // Thrown clear: a pure gravity arc, then splashdown with
                        // a little cartoon rebound.
                        m_tug.vy += h * 2.4 * pdt;
                        m_tug.py += m_tug.vy * pdt;
                        if (m_tug.py >= waterY) {
                            const qreal impact = m_tug.vy;
                            m_tug.py = waterY;
                            m_tug.vy = -impact * 0.28;
                        }
                    } else {
                        // Buoyancy spring: he FLOATS after the water — lagging,
                        // overshooting, bobbing — instead of riding it like
                        // glue. A hard fast riser flings him off the surface;
                        // the jump is emergent, so only real hits launch him.
                        m_tug.vy += ((waterY - m_tug.py) * 46.0 - m_tug.vy * 6.5) * pdt;
                        m_tug.py += m_tug.vy * pdt;
                        if (m_tug.py > waterY + s * 0.10) m_tug.py = waterY + s * 0.10;
                    }
                    // Smoothed pitch: eases toward the water slope (much less
                    // of it mid-air), never snapping frame to frame.
                    const qreal dx = 0.02;
                    const qreal yl = h * ly.baseFrac - swellAt(m_tug.x - dx);
                    const qreal yr = h * ly.baseFrac - swellAt(m_tug.x + dx);
                    qreal tiltTgt = std::atan2(yr - yl, 2.0 * dx * w) * (180.0 / kPi);
                    if (airborne) tiltTgt *= 0.25;
                    m_tug.tilt += (tiltTgt - m_tug.tilt) * 0.18;
                    // Puff from wherever the funnel actually IS right now, so
                    // his bounces write visible kinks into the trail.
                    m_tug.smokeT += pdt;
                    if (m_tug.smokeT > 0.16 && m_smoke.size() < 14) {
                        m_tug.smokeT = 0.0;
                        m_smoke.push_back({ m_tug.x * w - m_tug.dir * 0.26 * s,
                                            m_tug.py - 0.50 * s,
                                            0.0, frand(-0.15, 0.35) });
                    }
                    m_tug.lastT = m_t;
                }
                drawTugboat(p, m_tug.x * w, m_tug.py, s, m_tug.tilt, m_tug.dir, na);
            }
            // His trail: world-space puffs swelling and thinning as they age
            // (drawn with his sheet, so nearer water passes in front).
            for (const Puff& pf : m_smoke) {
                const qreal k = pf.age / 1.6;
                const int   a = int(lerp(130, 90, na) * (1.0 - k));
                QColor sc = lerpColor(QColor(228, 231, 236), QColor(165, 172, 184), na);
                sc.setAlpha(a);
                p.setBrush(sc);
                p.drawEllipse(QPointF(pf.x, pf.y), s * (0.055 + 0.115 * k), s * (0.055 + 0.115 * k));
            }
        }
    }
}

// How strongly the lighthouse lamp lights a point right now (0 = in the dark).
// Mirrors drawLighthouse's beam geometry — two opposed horizontal cones from
// the lantern, on-screen reach 0.55w * |cos(plan angle)|, fan opening at 0.075
// per unit distance — with brightness fading toward the beam tip like the
// drawn gradient does. Night-only; used to light the gulls out of their
// after-dark silhouettes as the beam sweeps them.
qreal Visualizer::beamLightAt(qreal x, qreal y) const {
    const qreal na = (m_cycleNight >= 0.0) ? m_cycleNight : (m_mode == Mode::Night ? 1.0 : 0.0);
    if (na <= 0.001) return 0.0;
    const qreal w = width(), h = height();
    const QPointF lc(m_lightBase.x(), m_lightBase.y() - h * 0.113); // ~lamp height on the taller tower

    // SIDE SWEEP: the horizontal beam reach as the optic turns across the screen.
    // Note this collapses to zero when the optic faces the viewer (cos -> 0), so
    // on its own it can never light a gull in FRONT of the tower during a flash.
    const qreal ext = w * 0.55 * std::abs(std::cos(m_beamA));
    qreal sweepLit = 0.0;
    const qreal dxg = std::abs(x - lc.x());
    if (dxg >= w * 0.02 && dxg <= ext) {
        const qreal fan = dxg * 0.075 + h * 0.012; // slack so a gull's body counts, not its exact centre
        const qreal dyg = std::abs(y - lc.y());
        if (dyg < fan) {
            const qreal core  = 1.0 - dyg / fan; // 1 on the beam axis -> 0 at its edge
            const qreal reach = 1.0 - dxg / ext; // dimmer toward the tip
            sweepLit = core * (0.35 + 0.65 * reach);
        }
    }

    // HEAD-ON FLASH: when the optic faces the viewer it floods FORWARD (the same
    // |sin|^12 pulse that flashes the lamp itself), lighting gulls in front of the
    // tower in a soft radial pool around the lamp — exactly the case the side
    // sweep misses. Taking the max means a gull is lit whether the beam catches it
    // from the side or head-on.
    const qreal flash = std::pow(std::abs(std::sin(m_beamA)), 12.0);
    qreal flashLit = 0.0;
    if (flash > 0.01) {
        const qreal dx = (x - lc.x()) / (w * 0.17);
        const qreal dy = (y - lc.y()) / (h * 0.20);
        flashLit = flash * qBound(0.0, 1.0 - std::sqrt(dx * dx + dy * dy), 1.0);
    }

    return qBound(0.0, qMax(sweepLit, flashLit) * na, 1.0); // fade the lighting with nightness
}

// The little red-and-white tugboat: a rare guest who chugs across the swells.
// (x, y) is the waterline under him, s his height scale (smaller on farther
// sheets), tiltDeg the local wave slope, dir his travel direction (bow leads).
void Visualizer::drawTugboat(QPainter& p, qreal x, qreal y, qreal s, qreal tiltDeg, int dir, qreal na) {
    // Day/night colours lerp by na (0..1): exact endpoints for the fixed scenes,
    // a smooth dusk in between for Cycle.
    auto C = [na](const QColor& day, const QColor& night) { return lerpColor(day, night, na); };
    p.save();
    p.translate(x, y);
    p.rotate(tiltDeg);          // pitch with the water (screen-space, so it
    if (dir > 0) p.scale(-1.0, 1.0); // survives the mirror); hull is drawn
                                     // bow-LEFT natively, flipped to lead.
    p.setPen(Qt::NoPen);
    // Red hull, raked bow, squared stern; the bottom sits below the waterline
    // so he reads as in the water, not on it.
    QPainterPath hull;
    hull.moveTo(-0.52 * s, -0.16 * s); // bow tip
    hull.lineTo( 0.46 * s, -0.16 * s); // deck line
    hull.lineTo( 0.38 * s,  0.16 * s);
    hull.lineTo(-0.30 * s,  0.16 * s); // keel back to the raked bow
    hull.closeSubpath();
    p.setBrush(C(QColor("#c23b34"), QColor("#7e3336")));
    p.drawPath(hull);
    // White wheelhouse amidships, then the funnel aft — red with a dark cap.
    p.setBrush(C(QColor("#f2efe6"), QColor("#a8adb5")));
    p.drawRect(QRectF(-0.20 * s, -0.44 * s, 0.34 * s, 0.28 * s));
    p.setBrush(C(QColor("#c23b34"), QColor("#7e3336")));
    p.drawRect(QRectF(0.20 * s, -0.40 * s, 0.11 * s, 0.24 * s));
    p.setBrush(C(QColor("#33383f"), QColor("#1c1f27")));
    p.drawRect(QRectF(0.20 * s, -0.40 * s, 0.11 * s, 0.07 * s));
    // Wheelhouse window: dark glass by day, warm lamplight at night.
    // (His smoke is world-space particles, emitted and drawn by drawShore —
    // not part of this rigid body, so the trail reacts to his motion.)
    p.setBrush(C(QColor("#3a4550"), QColor(255, 214, 140, 235)));
    p.drawRect(QRectF(-0.14 * s, -0.39 * s, 0.10 * s, 0.10 * s));
    p.restore();
}

void Visualizer::drawLighthouse(QPainter& p, qreal na) {
    // na 0..1 fades day->night. Every masonry/rock/dome tint LERPS by na, and the
    // lamp (glass<->lit) and swept beam fade by na too, so the whole point fades in
    // step with the rest of the shore — no midpoint pop. Fixed scenes pass exactly 0
    // or 1, so they render as before. C(day, night) picks the tint for the nightness.
    auto C = [na](const QColor& day, const QColor& nite) { return lerpColor(day, nite, na); };
    const qreal w = width(), h = height();
    p.setPen(Qt::NoPen);

    // The rocks breaking the waterline, then the point over them. Hazy, bluish
    // tones — it's all far away.
    p.setBrush(C(QColor("#4e5a68"), QColor("#0b101c")));
    for (const QPolygonF& r : m_rocks) p.drawPolygon(r);
    p.setBrush(C(QColor("#5a6472"), QColor("#0d1322")));
    p.drawPolygon(m_cliff);

    // The lighthouse, at distance scale. All proportions hang off its base point.
    const qreal bx = m_lightBase.x(), by = m_lightBase.y();
    const qreal towerH = h * 0.106;               // a touch taller, so it stands over the island
    const qreal ty = by - towerH;                 // top of the masonry
    const qreal bw = h * 0.0131, tw = h * 0.0085; // half-widths at base / top
    auto halfAt = [&](qreal y) { return bw + (tw - bw) * (by - y) / towerH; };

    // Tapered tower, shaded across its width so it reads as round.
    QLinearGradient shade(bx - bw, 0, bx + bw, 0);
    shade.setColorAt(0.0,  C(QColor("#f0ecdf"), QColor("#8d93a3")));
    shade.setColorAt(0.55, C(QColor("#ddd6c6"), QColor("#6a7080")));
    shade.setColorAt(1.0,  C(QColor("#b2adA0"), QColor("#474d5c")));
    QPolygonF tower;
    tower << QPointF(bx - bw, by) << QPointF(bx + bw, by)
          << QPointF(bx + tw, ty) << QPointF(bx - tw, ty);
    p.setBrush(shade);
    p.drawPolygon(tower);

    // Two red bands around the tower (clipped to the taper by halfAt).
    p.setBrush(C(QColor("#c14238"), QColor("#6b3540")));
    for (qreal f : { 0.32, 0.62 }) {
        const qreal y1 = by - towerH * f, y0 = y1 + towerH * 0.13;
        QPolygonF band;
        band << QPointF(bx - halfAt(y0), y0) << QPointF(bx + halfAt(y0), y0)
             << QPointF(bx + halfAt(y1), y1) << QPointF(bx - halfAt(y1), y1);
        p.drawPolygon(band);
    }

    // Gallery deck, lantern room, dome.
    const qreal lw = tw * 0.85, lh = h * 0.0135;
    p.setBrush(C(QColor("#2c2a33"), QColor("#20232e")));
    p.drawRect(QRectF(bx - tw * 1.65, ty - h * 0.0022, tw * 3.3, h * 0.0044));
    // How directly the optic faces us right now (1 = a beam is aimed straight at
    // the viewer). Raised to a high power it becomes the FLASH: a tight bloom
    // only while the beam crosses our line of sight, once per half turn.
    const qreal flashK = (na > 0.0) ? std::pow(std::abs(std::sin(m_beamA)), 12.0) : 0.0;
    const QRectF lantern(bx - lw, ty - h * 0.0022 - lh, lw * 2, lh);
    // Cross-fade the lamp so it never pops: the day GLASS (translucent panes with a
    // metal frame + mullion) fades OUT as the lit lamp fades IN with na. Exact glass
    // at na=0, exact lit lamp at na=1.
    if (na < 1.0) {
        p.setBrush(QColor(170, 205, 228, int(84 * (1.0 - na))));
        p.drawRect(lantern);
        const qreal bar = qMax(1.0, h / 950.0);
        QColor frame("#3a3f4a"); frame.setAlpha(int(255 * (1.0 - na)));
        p.setPen(QPen(frame, bar));
        p.setBrush(Qt::NoBrush);
        p.drawRect(lantern);
        p.drawLine(QPointF(bx, lantern.top()), QPointF(bx, lantern.bottom()));
        p.setPen(Qt::NoPen);
    }
    if (na > 0.0) {
        // The lit lamp, surging as the optic turns our way (alpha eases in by na).
        p.setBrush(QColor(255, int(205 + 30 * flashK), 135, int((150 + 105 * flashK) * na)));
        p.drawRect(lantern);
    }
    QPainterPath dome;
    const qreal dy = ty - h * 0.0022 - lh;
    dome.moveTo(bx - tw, dy);
    dome.arcTo(QRectF(bx - tw, dy - tw, tw * 2, tw * 2), 0, 180);
    p.setBrush(C(QColor("#c14238"), QColor("#6b3540")));
    p.drawPath(dome);

    if (na <= 0.001) return; // day: no beam (matches the fixed scenes exactly)

    // The optic spins in PLAN — a fixed horizontal plane, like the real thing —
    // and the screen shows its projection: two opposed beams reaching out to the
    // SIDES, foreshortening as the lens turns off-axis (reach = L*|cos|), and a
    // tight FLASH at the lantern the moment a beam crosses our line of sight
    // (|sin| -> 1). step()/beat() lock the rotation to the tempo, so the flash
    // rides the song's pulse — the lamp never chases individual sounds. Additive
    // composition; drawn with the distant scenery so nearer waves pass in front.
    const QPointF lc(bx, dy + lh * 0.45);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    const qreal ext = w * 0.55 * std::abs(std::cos(m_beamA)); // on-screen beam reach
    if (ext > w * 0.02) {
        for (int side = -1; side <= 1; side += 2) {
            const qreal tipX = lc.x() + side * ext;
            const qreal fan  = ext * 0.075; // slight vertical fan toward the tip
            QPainterPath cone;
            cone.moveTo(lc);
            cone.lineTo(QPointF(tipX, lc.y() - fan));
            cone.lineTo(QPointF(tipX, lc.y() + fan));
            cone.closeSubpath();
            QLinearGradient bg(lc, QPointF(tipX, lc.y()));
            bg.setColorAt(0.0, QColor(255, 236, 170, int(95 * na)));
            bg.setColorAt(1.0, QColor(255, 236, 170, 0));
            p.fillPath(cone, bg);
        }
    }
    // The viewer-facing flash: blooms and dies in a fraction of a beat.
    const qreal haloR = h * (0.030 + 0.085 * flashK);
    QRadialGradient halo(lc, haloR);
    halo.setColorAt(0.0, QColor(255, 224, 155, int((70 + 185 * flashK) * na)));
    halo.setColorAt(1.0, QColor(255, 224, 155, 0));
    p.setBrush(halo);
    p.drawEllipse(lc, haloR, haloR);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void Visualizer::drawClouds(QPainter& p) {
    p.setPen(Qt::NoPen);
    const qreal na = (m_cycleNight >= 0.0) ? m_cycleNight : (m_mode == Mode::Night ? 1.0 : 0.0);
    const int alpha = int(lerp(30, 11, na)); // night clouds fade to barely-there wisps
    for (const Cloud& c : m_clouds) {
        for (int j = 0; j < c.puffs.size(); ++j) {
            const qreal r = c.pr[j] * c.scale;
            p.setBrush(QColor(255, 255, 255, alpha));
            p.drawEllipse(QPointF(c.x + c.puffs[j].x() * c.scale,
                                  c.y + c.puffs[j].y() * c.scale),
                          r, r * 0.62);
        }
    }
}
