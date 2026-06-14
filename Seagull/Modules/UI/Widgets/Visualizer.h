#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <QWidget>
#include <QVector>
#include <QColor>
#include <QPixmap>
#include <QPointF>

class QTimer;
class QResizeEvent;

// The audio visualizer: a Qt-drawn sky (the PRIMARY reaction is the sky colour +
// gradient bleed, driven by the audio level and the bass/mid/treble balance)
// with parallax clouds and seagulls that drift across. The number of gulls
// tracks loudness; they bob gently and flap at the gif's own native speed. On
// end-of-file they spin and fall out of the sky.
//
// Data-source agnostic: consumes a normalised level, a 3-band spectrum, and beat
// pulses. setDemoMode(true) self-synthesises them until the real audio tap is on.
// Hosted by VideoPlayer as a top-level overlay over the audio surface.
class Visualizer : public QWidget {
    Q_OBJECT
public:
    explicit Visualizer(QWidget* parent = nullptr);

public slots:
    void setAudioLevel(float level01);                   // 0..1 overall level
    void setSpectrum(float bass, float mid, float treble); // 0..1 per band
    void beat();                                         // a detected beat / onset
    void setDemoMode(bool on);                           // self-drive until real audio
    void setBehavior(const QString& name);               // gull behaviour: Drift/Reverse/Swooping/Flocking
    void setMaxGulls(int n);                             // perf cap on the flock size
    void setMode(const QString& name);                   // "Seagull Sky" or "Seagull Waves"
    void setPaused(bool on);                             // freeze/resume the animation
    void triggerDeath();                                 // EOF: gulls spin and fall
    void reviveGulls();                                  // playback resumed: fresh flock

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // rescale elements to the new size

private:
    enum class Mode { Sky, Waves };
    enum class GullBehavior { Drift, Reverse, Swooping, Flocking };

    struct Gull  { qreal x, y, size, speed, phase, flap; int foff;
                   qreal rot, spin, vy, yoff; bool dying;
                   qreal swoopP, swoopAmp; }; // swoopP: 0..1 progress, <0 = flying level
    // Each cloud is a little cluster of puffs (varied count/offset/size) so no
    // two look alike.
    struct Cloud { qreal x, y, scale, speed; QVector<QPointF> puffs; QVector<qreal> pr; };

    void step();
    void seed();
    void seedClouds();    // (re)build clouds sized relative to the current widget
    void recycleGull(Gull& g);
    void drawGull(QPainter& p, const Gull& g);
    void drawSky(QPainter& p);    // Seagull Sky: smooth gradient bleed
    void drawWaves(QPainter& p);  // Seagull Waves: ocean waves
    void drawClouds(QPainter& p);
    void loadGullFrames();

    Mode m_mode = Mode::Sky;
    GullBehavior m_behavior = GullBehavior::Drift;
    int  m_maxGulls = 14;          // perf cap

    QVector<QPixmap> m_gullFrames; // animated gull frames (faces left natively; flipped per direction)
    QVector<int>     m_frameDelays; // per-frame display time (ms) — native gif speed
    int   m_frameIdx = 0;
    qreal m_frameAcc = 0.0;

    QTimer* m_timer = nullptr;
    QVector<Gull>  m_gulls;
    QVector<Cloud> m_clouds;

    // Audio inputs, eased toward their targets (responsive = low latency).
    qreal m_level   = 0.0, m_tLevel   = 0.0;
    qreal m_bass    = 0.0, m_tBass    = 0.0;
    qreal m_mid     = 0.0, m_tMid     = 0.0;
    qreal m_treble  = 0.0, m_tTreble  = 0.0;
    qreal m_colorTone = 0.5; // SLOW-eased spectral balance -> bottom sky hue

    qreal m_beatFlash = 0.0; // decays after each beat (gentle drift gust)
    qreal m_t         = 0.0; // global clock

    // Tempo: eased interval between bass-drum beats -> one stable glide speed.
    qreal m_beatInterval = 0.5; // seconds (~120 BPM default)
    qreal m_lastBeatT    = -1.0;
    qreal m_tempoSpeed   = 1.0; // glide multiplier derived from tempo
    qreal m_demoBeatT = 0.0; // demo beat accumulator
    qreal m_spawnT    = 0.0; // spacing for spawning gulls toward the target count
    bool  m_demo      = false;
    bool  m_dying     = false; // EOF death spiral in progress
    bool  m_paused    = false; // playback paused -> freeze the animation
};

#endif // VISUALIZER_H
