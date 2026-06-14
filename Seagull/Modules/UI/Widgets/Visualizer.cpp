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
#include <cmath>

namespace {
    constexpr int kFps      = 60;
    constexpr int kMaxGulls = 14;
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
        rgba = rgba.mirrored(true, false);
        rgba = rgba.scaledToHeight(160, Qt::SmoothTransformation);
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
    m_beatFlash = 1.0;

    // Tempo from the gap between bass beats. Only accept musical intervals
    // (~40-200 BPM) and ease the estimate so the glide speed stays consistent
    // rather than twitching on every kick.
    if (m_lastBeatT >= 0.0) {
        const qreal interval = m_t - m_lastBeatT;
        if (interval > 0.28 && interval < 1.6)
            m_beatInterval += (interval - m_beatInterval) * 0.30; // lock on quickly
    }
    m_lastBeatT = m_t;
    // Exaggerate the deviation from ~120 BPM so slow vs fast songs glide visibly
    // differently (most songs cluster near 120, so the raw spread is small).
    const qreal raw = 0.5 / m_beatInterval; // 1.0 at ~120 BPM
    m_tempoSpeed = qBound(0.45, 1.0 + (raw - 1.0) * 2.0, 2.4);

    if (!m_dying && m_gulls.size() < kMaxGulls) { // a beat sends an extra gull across
        Gull g; recycleGull(g, true);
        m_gulls.push_back(g);
    }
}

void Visualizer::setDemoMode(bool on) { m_demo = on; m_demoBeatT = 0.0; }
void Visualizer::setGullStyle(bool animated) { m_useGif = animated; update(); }
void Visualizer::setMode(const QString& name) {
    m_mode = name.contains(QStringLiteral("Waves"), Qt::CaseInsensitive) ? Mode::Waves : Mode::Sky;
    update();
}

void Visualizer::setPaused(bool on) {
    m_paused = on;
    if (on) m_timer->stop();                 // freeze where it is
    else if (isVisible()) m_timer->start();  // resume
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
    if (!m_dying) return; // resuming from pause shouldn't disturb a living flock
    m_dying = false;
    m_gulls.clear();      // fresh flock spawns back in with the music
    m_spawnT = 0.0;
}

void Visualizer::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    seed();
    if (!m_paused) m_timer->start();
}

void Visualizer::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_timer->stop();
}

void Visualizer::seed() {
    seedClouds();
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
    seedClouds(); // rescale clouds to the new size; the flock keeps flying
}

void Visualizer::recycleGull(Gull& g, bool fromLeft) {
    const qreal w = qMax(1, width()), h = qMax(1, height());
    g.size  = frand(h * 0.02, h * 0.06);   // smaller = farther away
    g.x     = fromLeft ? -g.size : frand(0, w);
    g.y     = frand(h * 0.08, h * 0.55);   // up in the sky, not close/low
    g.speed = frand(0.4, 1.4) * (g.size / (h * 0.045));
    g.phase = frand(0, 6.28);
    g.flap  = frand(0.8, 1.3);
    g.foff  = QRandomGenerator::global()->bounded(qMax(1, m_gullFrames.size()));
    g.rot = 0.0; g.spin = 0.0; g.vy = 0.0; g.dying = false;
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
    m_treble = easeAR(m_treble, m_tTreble, 0.30, 0.11);
    m_beatFlash *= 0.92;

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

    // Flock size tracks loudness (unless they're falling out of the sky).
    const int desired = m_dying ? 0 : int(std::lround(m_level * kMaxGulls));
    m_spawnT += dt;
    if (!m_dying && m_gulls.size() < desired && m_spawnT > 0.12) {
        Gull g; recycleGull(g, true);
        m_gulls.push_back(g);
        m_spawnT = 0.0;
    }

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
        g.x += g.speed * 1.3 * m_tempoSpeed; // steady glide, paced by the song's tempo
        if (g.x - g.size > w) {
            if (m_gulls.size() > desired) { m_gulls.removeAt(i); --i; } // thin to the target
            else recycleGull(g, true);
        }
    }
    update();
}

void Visualizer::drawGull(QPainter& p, const Gull& g) {
    // Up-and-down bob like a real bird in flight (frozen while it's tumbling down).
    const qreal bob = g.dying ? 0.0 : std::sin(g.phase) * g.size * 0.20;
    p.save();
    p.translate(g.x, g.y + bob);
    if (g.rot != 0.0) p.rotate(g.rot);

    if (m_useGif && !m_gullFrames.isEmpty()) {
        const QPixmap& fr = m_gullFrames[(m_frameIdx + g.foff) % m_gullFrames.size()];
        const qreal tw = g.size * 2.6; // wingspan
        const qreal th = tw * fr.height() / fr.width();
        p.drawPixmap(QRectF(-tw / 2, -th / 2, tw, th), fr, fr.rect());
        p.restore();
        return;
    }

    // Painted white-outline gull (settings choice, or gif fallback).
    const qreal s = g.size;
    const qreal dip = (0.34 + 0.18 * std::sin(g.phase)) * s; // gentle wing sweep
    QPainterPath path;
    path.moveTo(-s, 0);
    path.quadTo(-s * 0.5, -dip, 0, 0);
    path.quadTo(s * 0.5, -dip, s, 0);
    QPen pen(QColor(255, 255, 255, 235));
    pen.setWidthF(qMax<qreal>(1.6, s * 0.11));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}

void Visualizer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_mode == Mode::Waves) drawWaves(p);
    else                       drawSky(p);

    drawClouds(p);
    for (const Gull& g : m_gulls) drawGull(p, g);
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

    p.setRenderHint(QPainter::Antialiasing, false);
    const int N = 80;
    const qreal colStep = w / N;
    for (int i = 0; i < N; ++i) {
        const qreal xn = (i + 0.5) / N;
        // Boundary position from the top (0..1): a little bleed even when silent,
        // most of the sky when that band is loud.
        const qreal warmTop = qBound(0.18, 1.0 - (0.12 + 0.72 * bandFrac(xn)), 0.86);
        QLinearGradient g(0, 0, 0, h);
        g.setColorAt(0.0, cTop);
        g.setColorAt(qBound(0.0, warmTop * 0.6, 1.0), cHigh);
        g.setColorAt(warmTop, cRose);
        g.setColorAt(qBound(0.0, warmTop + (1.0 - warmTop) * 0.5, 1.0), cAmb);
        g.setColorAt(1.0, cGold);
        p.fillRect(QRectF(i * colStep, 0, colStep + 1.0, h), g);
    }
    p.setRenderHint(QPainter::Antialiasing, true);
}

void Visualizer::drawWaves(QPainter& p) {
    const qreal w = width(), h = height();

    // Sea sky: cool gradient behind the water.
    QLinearGradient sky(0, 0, 0, h);
    sky.setColorAt(0.0, QColor("#0d2238"));
    sky.setColorAt(1.0, QColor("#2a6f86"));
    p.fillRect(rect(), sky);

    // The water SITS STILL; it only swells as a visual-EQ response to the sound,
    // and each wave reacts to ONE band in ONE zone of the screen:
    //   front  (drawn last,  lowest,  lightest) = BASS,   swells on the LEFT
    //   middle                                  = MIDS,   swells in the CENTRE
    //   back   (drawn first, highest, darkest)  = TREBLE, swells on the RIGHT
    // A broad raised-cosine window keeps each band's motion in its zone; the swell
    // (and its ripple) scale with that band, so quiet => flat calm sea.
    const qreal maxH = h * 0.20;
    auto window = [](qreal xn, qreal centre, qreal halfWidth) -> qreal {
        const qreal d = std::abs(xn - centre) / halfWidth;
        if (d >= 1.0) return 0.0;
        return 0.5 * (1.0 + std::cos(d * 3.14159265358979)); // 1 at centre -> 0 at edge
    };
    const qreal hw = 0.60;

    struct Layer { qreal baseFrac, band, centre, ripFreq, ripPhase, ampMul; QColor col; };
    const Layer layers[3] = {
        { 0.72, m_treble, 0.90, 19.0, 0.0, 1.0, QColor("#16566e") }, // back   = treble, far right
        { 0.82, m_mid,    0.62, 14.0, 1.6, 1.0, QColor("#1f7d92") }, // middle = mids,   right of centre
        { 0.92, m_bass,   0.16, 10.0, 3.1, 2.3, QColor("#3aa6b3") }, // front  = bass,   left (taller, owns the left)
    };
    p.setPen(Qt::NoPen);
    const int N = 80;
    for (const Layer& ly : layers) {
        QPainterPath wave;
        wave.moveTo(0, h);
        for (int i = 0; i <= N; ++i) {
            const qreal xn = qreal(i) / N;
            const qreal env = ly.band * window(xn, ly.centre, hw); // 0 outside zone / when quiet
            const qreal swell = env * ly.ampMul * (maxH + h * 0.06 * std::sin(xn * ly.ripFreq + ly.ripPhase));
            wave.lineTo(xn * w, h * ly.baseFrac - swell);
        }
        wave.lineTo(w, h);
        wave.closeSubpath();
        p.fillPath(wave, ly.col);
    }
}

void Visualizer::drawClouds(QPainter& p) {
    p.setPen(Qt::NoPen);
    for (const Cloud& c : m_clouds) {
        for (int j = 0; j < c.puffs.size(); ++j) {
            const qreal r = c.pr[j] * c.scale;
            p.setBrush(QColor(255, 255, 255, 30));
            p.drawEllipse(QPointF(c.x + c.puffs[j].x() * c.scale,
                                  c.y + c.puffs[j].y() * c.scale),
                          r, r * 0.62);
        }
    }
}
