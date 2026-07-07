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

    constexpr qreal kSwoopBottom = 0.35; // fraction of the swoop spent diving
    constexpr int   kDiveFrame   = 1;    // wings out level (mid-stroke): the dive + climb pose
    constexpr int   kGlideFrame  = 2;    // wings fully spread, tips drooped: the bottom-of-arc glide
    constexpr qreal kGlideDepth  = 0.80; // how deep into the arc the glide pose takes over
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
}

Visualizer::Visualizer(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false); // we paint an opaque sky ourselves
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
    // Anything unrecognised (including a legacy saved "Seagull Sky") lands on Morning.
    if      (name.contains(QStringLiteral("Night"), Qt::CaseInsensitive)) m_mode = Mode::Night;
    else if (name.contains(QStringLiteral("Waves"), Qt::CaseInsensitive)) m_mode = Mode::Waves;
    else                                                                  m_mode = Mode::Morning;
    update();
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
    seedClouds();  // rescale clouds to the new size; the flock keeps flying
    seedScenery(); // and rebuild the shore scene's geometry for the new size

    // Rescale the LIVING flock too: gull sizes/positions/speeds are absolute
    // pixels seeded from the old dimensions, so a fullscreen jump used to leave
    // the present gulls tiny while fresh spawns (sized off the new height)
    // dwarfed them. Proportional rescale keeps everyone consistent.
    const QSize old = event->oldSize();
    if (old.width() > 0 && old.height() > 0 && width() > 0 && height() > 0) {
        const qreal fx = qreal(width()) / old.width();
        const qreal fy = qreal(height()) / old.height();
        for (Gull& g : m_gulls) {
            g.x *= fx; g.y *= fy;
            g.size *= fy; g.speed *= fy;
            g.swoopAmp *= fy; g.vy *= fy;
        }
    }
}

void Visualizer::seedScenery() {
    const qreal w = qMax(1, width()), h = qMax(1, height());

    // Night stars: scattered over the sky, denser up high, each with its own
    // twinkle phase. Seeded, so the constellation holds still frame to frame.
    m_stars.clear();
    const int nStars = int(qBound(50.0, w * h / 14000.0, 160.0));
    for (int i = 0; i < nStars; ++i) {
        Star s;
        s.x  = frand(0, w);
        s.y  = frand(0, h * 0.62) * frand(0.35, 1.0); // bias upward
        s.r  = frand(0.6, 1.7) * qMax(1.0, h / 640.0);
        s.tw = frand(0, 6.28);
        m_stars.push_back(s);
    }

    // The distant shoreline along the horizon: a gently rolling grass line above
    // a sliver of sand at the waterline. Sampled from smooth sines with random
    // phases so every launch rolls a little differently, and trees sit EXACTLY
    // on the sampled line.
    const qreal ph1 = frand(0, 6.28), ph2 = frand(0, 6.28);
    auto grassEdge = [&](qreal xn) {
        return h * (0.664 + 0.011 * std::sin(xn * 6.8 + ph1)
                          + 0.006 * std::sin(xn * 14.7 + ph2));
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
    const int nTrees = 8 + QRandomGenerator::global()->bounded(5);
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
    m_cliff << QPointF(-4, 0.745 * h)               // closed just under the waterline
            << QPointF(-4, jy(0.630))
            << QPointF(jx(0.028), jy(0.612))
            << QPointF(jx(0.092), jy(0.610))        // the plateau
            << QPointF(jx(0.112), jy(0.648))        // then a craggy little face
            << QPointF(jx(0.103), jy(0.682))
            << QPointF(jx(0.128), jy(0.712))
            << QPointF(0.132 * w, 0.745 * h);
    m_lightBase = QPointF(0.058 * w, 0.614 * h);    // on the plateau, clear of its edges

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
    g.size  = frand(h * 0.02, h * 0.06);          // smaller = farther away
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
    // direction, never making the same crossing twice in a row.
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
        qreal surge = 1.0;
        if (g.swoopP >= 0.0) surge = 1.0 + 0.9 * swoopDepth(g.swoopP);
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
            g.swoopAmp = frand(0.12, 0.26) * h;
            g.swoopDur = frand(2.0, 2.8); // seconds; varied so the flock doesn't sync up
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
        const qreal vx = g.speed * (0.9 + 1.1 * m_tempoSpeed) * (1.0 + 0.9 * depth) * 60.0;
        const qreal vy = slope * g.swoopAmp / g.swoopDur;
        tilt = qBound(-50.0, std::atan2(vy, qMax(1.0, vx)) * (180.0 / 3.14159265358979) * 0.75, 50.0) * dir;
    } else if (m_behavior == GullBehavior::Flocking) {
        vOff = std::sin(g.phase) * g.size * 0.10;
    } else {
        vOff = std::sin(g.phase) * g.size * 0.20;
    }

    // Wings through a swoop: held out level (kDiveFrame) in the dive, snapped to
    // the full spread (kGlideFrame) gliding through the bottom of the arc, back
    // to level on the way up; normal flapping resumes only once the swoop ends.
    int fi = (m_frameIdx + g.foff) % m_gullFrames.size();
    if (swooping)
        fi = qMin(swoopDepth(g.swoopP) >= kGlideDepth ? kGlideFrame : kDiveFrame,
                  int(m_gullFrames.size()) - 1);
    const QPixmap& fr = m_gullFrames[fi];
    const qreal tw = g.size * 2.6; // wingspan
    const qreal th = tw * fr.height() / fr.width();

    p.save();
    p.translate(g.x, g.y + vOff);
    if (g.rot != 0.0 || tilt != 0.0) p.rotate(g.rot + tilt);
    if (dir > 0) p.scale(-1.0, 1.0); // frames face left natively; flip to face travel direction
    p.drawPixmap(QRectF(-tw / 2, -th / 2, tw, th), fr, fr.rect());
    p.restore();
}

void Visualizer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_mode == Mode::Morning) drawSky(p);   // sunrise EQ sky over the shore
    else                         drawWaves(p); // flat day / starry night over the shore

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

void Visualizer::drawSky(QPainter& p) {
    const qreal w = width(), h = height();
    const qreal e = qBound(0.0, m_level, 1.0);

    // Slight colour shift with the music (saturation only — never washes white).
    auto glow = [e](const QColor& base) {
        float hh, s, l, a; base.getHslF(&hh, &s, &l, &a);
        s = float(qBound(0.0, double(s) * (0.72 + 0.38 * e), 1.0));
        l = float(qBound(0.0, double(l) * (0.92 + 0.08 * e), 0.85));
        return QColor::fromHslF(hh, s, l);
    };
    const QColor cTop  = glow(QColor("#1a2350")); // deep indigo (top)
    const QColor cHigh = glow(QColor("#5b3f87")); // violet
    const QColor cRose = glow(QColor("#c75c87")); // pink (the bleed boundary)
    const QColor cAmb  = glow(QColor("#f0975a")); // orange
    const QColor cGold = glow(QColor("#ffd79a")); // gold horizon

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

    // The gradient now spans only down to the waterline — the gold horizon lands
    // where sea meets sky, and the shore scene + waves own everything below it
    // (the gradient's final stop pads the covered remainder).
    p.setRenderHint(QPainter::Antialiasing, false);
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
        p.fillRect(QRectF(i * colStep, 0, colStep + 1.0, h), g);
    }
    p.setRenderHint(QPainter::Antialiasing, true);

    drawShore(p, /*night=*/false); // the same coast, lit by the sunrise
}

void Visualizer::drawWaves(QPainter& p) {
    const bool night = (m_mode == Mode::Night);
    const qreal w = width(), h = height();

    // Sea sky: cool day gradient, or a deep star-field night.
    QLinearGradient sky(0, 0, 0, h);
    if (night) { sky.setColorAt(0.0, QColor("#050912")); sky.setColorAt(1.0, QColor("#13293e")); }
    else       { sky.setColorAt(0.0, QColor("#0d2238")); sky.setColorAt(1.0, QColor("#2a6f86")); }
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

    drawShore(p, night);
}

void Visualizer::drawShore(QPainter& p, bool night) {
    const qreal w = width(), h = height();

    // The distant shoreline, all of it AT the horizon behind the water: rolling
    // grass, then a just-visible sliver of sand at the waterline (calm — no wave
    // motion touches it), little pines on the grass line, and the lighthouse
    // point far off on the left. The waves are drawn after, in front.
    p.setPen(Qt::NoPen);
    p.setBrush(night ? QColor("#0a1a20") : QColor("#4e8266"));
    p.drawPolygon(m_grass);
    // The sand line, with the land's soft shadow falling on its top edge — that
    // shadow is what separates it from the similar-toned horizon sky (Morning's
    // gold sits right on this boundary).
    const QRectF sandR(0, h * 0.700, w, h * 0.028);
    p.fillRect(sandR, night ? QColor("#161e2e") : QColor("#c6b489"));
    QLinearGradient sandShadow(0, sandR.top(), 0, sandR.bottom());
    sandShadow.setColorAt(0.0,  QColor(40, 45, 60, night ? 130 : 120));
    sandShadow.setColorAt(0.80, QColor(40, 45, 60, 0));
    p.fillRect(sandR, sandShadow);
    p.setBrush(night ? QColor("#081420") : QColor("#2f6152"));
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
    drawLighthouse(p, night);

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
        { 0.72, m_treble, 1.45, 1.0,
          night ? QColor("#0b2a3c") : QColor("#16566e") }, // back   = treble
        { 0.82, m_mid,    1.30, 1.0,
          night ? QColor("#103a4e") : QColor("#1f7d92") }, // middle = mids
        { 0.92, m_bass,   1.00, 2.3,
          night ? QColor("#175066") : QColor("#3aa6b3") }, // front  = bass (taller)
    };
    p.setPen(Qt::NoPen);
    const int N = 80;
    for (int li = 0; li < 3; ++li) {
        const Layer& ly = layers[li];
        // Crest ceiling: the treetops for the two near sheets; the back sheet
        // stays low against the grass line — it's the FARTHEST water, and
        // crests towering over the horizon pines broke the depth read.
        const qreal ceilY = (li == 0) ? h * 0.665 : h * 0.635;
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
        if (night) { // moonlit crest line, brightening as its band swells
            p.setPen(QPen(QColor(185, 215, 250, int(28 + 70 * ly.band)), qMax(1.0, h / 700.0)));
            p.drawPath(crest);
            p.setPen(Qt::NoPen);
        }
        if (m_tug.active && m_tug.layer == li) {
            // The tugboat rides THIS sheet: drawn right after its water so
            // nearer sheets pass in front of him. He sits on the surface, bobs
            // gently, and pitches with the local wave slope.
            const qreal s  = h * ((li == 0) ? 0.026 : (li == 1) ? 0.038 : 0.052); // depth scale
            const qreal dx = 0.02;
            const qreal yb = h * ly.baseFrac - swellAt(m_tug.x) + std::sin(m_tug.bob) * s * 0.10;
            const qreal yl = h * ly.baseFrac - swellAt(m_tug.x - dx);
            const qreal yr = h * ly.baseFrac - swellAt(m_tug.x + dx);
            const qreal tilt = std::atan2(yr - yl, 2.0 * dx * w) * (180.0 / kPi);
            drawTugboat(p, m_tug.x * w, yb, s, tilt, m_tug.dir, night);
        }
    }
}

// The little red-and-white tugboat: a rare guest who chugs across the swells.
// (x, y) is the waterline under him, s his height scale (smaller on farther
// sheets), tiltDeg the local wave slope, dir his travel direction (bow leads).
void Visualizer::drawTugboat(QPainter& p, qreal x, qreal y, qreal s, qreal tiltDeg, int dir, bool night) {
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
    p.setBrush(night ? QColor("#7e3336") : QColor("#c23b34"));
    p.drawPath(hull);
    // White wheelhouse amidships, then the funnel aft — red with a dark cap.
    p.setBrush(night ? QColor("#a8adb5") : QColor("#f2efe6"));
    p.drawRect(QRectF(-0.20 * s, -0.44 * s, 0.34 * s, 0.28 * s));
    p.setBrush(night ? QColor("#7e3336") : QColor("#c23b34"));
    p.drawRect(QRectF(0.20 * s, -0.40 * s, 0.11 * s, 0.24 * s));
    p.setBrush(night ? QColor("#1c1f27") : QColor("#33383f"));
    p.drawRect(QRectF(0.20 * s, -0.40 * s, 0.11 * s, 0.07 * s));
    // Wheelhouse window: dark glass by day, warm lamplight at night.
    p.setBrush(night ? QColor(255, 214, 140, 235) : QColor("#3a4550"));
    p.drawRect(QRectF(-0.14 * s, -0.39 * s, 0.10 * s, 0.10 * s));
    p.restore();
}

void Visualizer::drawLighthouse(QPainter& p, bool night) {
    const qreal w = width(), h = height();
    p.setPen(Qt::NoPen);

    // The rocks breaking the waterline, then the point over them. Hazy, bluish
    // tones — it's all far away.
    p.setBrush(night ? QColor("#0b101c") : QColor("#4e5a68"));
    for (const QPolygonF& r : m_rocks) p.drawPolygon(r);
    p.setBrush(night ? QColor("#0d1322") : QColor("#5a6472"));
    p.drawPolygon(m_cliff);

    // The lighthouse, at distance scale. All proportions hang off its base point.
    const qreal bx = m_lightBase.x(), by = m_lightBase.y();
    const qreal towerH = h * 0.085;
    const qreal ty = by - towerH;                 // top of the masonry
    const qreal bw = h * 0.0105, tw = h * 0.0068; // half-widths at base / top
    auto halfAt = [&](qreal y) { return bw + (tw - bw) * (by - y) / towerH; };

    // Tapered tower, shaded across its width so it reads as round.
    QLinearGradient shade(bx - bw, 0, bx + bw, 0);
    if (night) { shade.setColorAt(0.0, QColor("#8d93a3")); shade.setColorAt(0.55, QColor("#6a7080")); shade.setColorAt(1.0, QColor("#474d5c")); }
    else       { shade.setColorAt(0.0, QColor("#f0ecdf")); shade.setColorAt(0.55, QColor("#ddd6c6")); shade.setColorAt(1.0, QColor("#b2adA0")); }
    QPolygonF tower;
    tower << QPointF(bx - bw, by) << QPointF(bx + bw, by)
          << QPointF(bx + tw, ty) << QPointF(bx - tw, ty);
    p.setBrush(shade);
    p.drawPolygon(tower);

    // Two red bands around the tower (clipped to the taper by halfAt).
    p.setBrush(night ? QColor("#6b3540") : QColor("#c14238"));
    for (qreal f : { 0.32, 0.62 }) {
        const qreal y1 = by - towerH * f, y0 = y1 + towerH * 0.13;
        QPolygonF band;
        band << QPointF(bx - halfAt(y0), y0) << QPointF(bx + halfAt(y0), y0)
             << QPointF(bx + halfAt(y1), y1) << QPointF(bx - halfAt(y1), y1);
        p.drawPolygon(band);
    }

    // Gallery deck, lantern room, dome.
    const qreal lw = tw * 0.85, lh = h * 0.011;
    p.setBrush(night ? QColor("#20232e") : QColor("#2c2a33"));
    p.drawRect(QRectF(bx - tw * 1.65, ty - h * 0.0022, tw * 3.3, h * 0.0044));
    // How directly the optic faces us right now (1 = a beam is aimed straight at
    // the viewer). Raised to a high power it becomes the FLASH: a tight bloom
    // only while the beam crosses our line of sight, once per half turn.
    const qreal flashK = night ? std::pow(std::abs(std::sin(m_beamA)), 12.0) : 0.0;
    const QRectF lantern(bx - lw, ty - h * 0.0022 - lh, lw * 2, lh);
    if (night) {
        // The lit lamp, surging as the optic turns our way.
        p.setBrush(QColor(255, int(205 + 30 * flashK), 135, int(150 + 105 * flashK)));
        p.drawRect(lantern);
    } else {
        // By day the lamp is off, so it reads as what it is: GLASS. Translucent
        // panes the sky shows through, held by a metal frame + centre mullion.
        p.setBrush(QColor(170, 205, 228, 84));
        p.drawRect(lantern);
        const qreal bar = qMax(1.0, h / 950.0);
        p.setPen(QPen(QColor("#3a3f4a"), bar));
        p.setBrush(Qt::NoBrush);
        p.drawRect(lantern);
        p.drawLine(QPointF(bx, lantern.top()), QPointF(bx, lantern.bottom()));
        p.setPen(Qt::NoPen);
    }
    QPainterPath dome;
    const qreal dy = ty - h * 0.0022 - lh;
    dome.moveTo(bx - tw, dy);
    dome.arcTo(QRectF(bx - tw, dy - tw, tw * 2, tw * 2), 0, 180);
    p.setBrush(night ? QColor("#6b3540") : QColor("#c14238"));
    p.drawPath(dome);

    if (!night) return;

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
            bg.setColorAt(0.0, QColor(255, 236, 170, 95));
            bg.setColorAt(1.0, QColor(255, 236, 170, 0));
            p.fillPath(cone, bg);
        }
    }
    // The viewer-facing flash: blooms and dies in a fraction of a beat.
    const qreal haloR = h * (0.030 + 0.085 * flashK);
    QRadialGradient halo(lc, haloR);
    halo.setColorAt(0.0, QColor(255, 224, 155, int(70 + 185 * flashK)));
    halo.setColorAt(1.0, QColor(255, 224, 155, 0));
    p.setBrush(halo);
    p.drawEllipse(lc, haloR, haloR);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void Visualizer::drawClouds(QPainter& p) {
    p.setPen(Qt::NoPen);
    const int alpha = (m_mode == Mode::Night) ? 11 : 30; // night clouds are barely-there wisps
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
