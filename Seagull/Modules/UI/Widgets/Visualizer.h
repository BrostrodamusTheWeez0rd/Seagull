#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <QWidget>
#include <QVector>
#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <array>

class QTimer;
class QResizeEvent;

// The audio visualizer: four Qt-drawn coastal scenes — Seagull Morning (sunrise
// gradient whose warm bleed is a per-band visual EQ), Seagull Day (bright blue
// afternoon), Seagull Dusk (a warm sunset over the sea) and Seagull Night (stars,
// moon, tempo-locked lighthouse lamp). All four share the distant shore (grass,
// sand, pines, lighthouse point) behind band-reactive waves, with parallax clouds
// and seagulls drifting across. The number of gulls tracks loudness; they bob
// gently and flap at the gif's own native speed. On end-of-file they spin and
// fall out of the sky.
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
    void setMode(const QString& name);                   // "Seagull Morning" / "Seagull Day" / "Seagull Dusk" / "Seagull Night" / "Seagull Cycle"
    void setProgress(qint64 posMs, qint64 durMs);        // song position/duration -> time-of-day for Cycle mode
    void setLighthouseBeats(int n);                      // beats per lighthouse flash (1 = every beat)
    void setPaused(bool on);                             // freeze/resume the animation
    void suspendRendering(bool on);                      // pause the render timer to free the GUI thread (e.g. Library build), independent of playback pause
    void triggerDeath();                                 // EOF: gulls spin and fall
    void reviveGulls();                                  // playback resumed: fresh flock

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // rescale elements to the new size

private:
    enum class Mode { Morning, Day, Dusk, Night };
    enum class GullBehavior { Drift, Reverse, Swooping, Flocking };

    // A time-of-day colour set for the sea scene. One table (paletteFor) holds
    // every colour that changes between the scenes, so retuning one means editing
    // one place instead of chasing "night ? a : b" ternaries.
    //   reactiveSky => the per-band EQ sky (drawSky, Morning/Dusk) built from the
    //                  five skyEq* stops; otherwise a static two/three-stop sky
    //                  (drawWaves, Day/Night) from skyTop/skyMid/skyBot.
    //   night       => the night-only extras (stars, moon, swept beam, dark gulls,
    //                  moonlit crest).
    //   skyMid invalid => a plain two-stop static sky.
    struct ScenePalette {
        bool   night = false;
        bool   reactiveSky = false;
        QColor skyTop, skyMid, skyBot;                             // static sky (Day/Night)
        QColor skyEqTop, skyEqHigh, skyEqRose, skyEqAmb, skyEqGold; // reactive EQ sky (Morning/Dusk)
        QColor grass, sand, trees; int sandShadowA = 120;
        QColor waterBack, waterMid, waterFront;
        QColor crestShimmer; // moonlight glinting along the wave crests at night;
                             // invalid on the day-lit scenes => a calm, matte sea
                             // with no crest outline.
    };
    ScenePalette paletteFor(Mode m) const;

    struct Gull  { qreal x, y, size, speed, phase, flap; int foff;
                   qreal rot, spin, vy, yoff; bool dying;
                   qreal swoopP, swoopAmp, swoopDur; }; // swoopP: 0..1 progress, <0 = flying level
    // Each cloud is a little cluster of puffs (varied count/offset/size) so no
    // two look alike.
    struct Cloud { qreal x, y, scale, speed; QVector<QPointF> puffs; QVector<qreal> pr; };
    struct Star  { qreal x, y, r, tw; };  // night-sky point; tw = twinkle phase
    struct Tree  { qreal x, y, s; };      // distant headland pine (base point + height)

    void step();
    void updateTimerState(); // run the animation only when visible, not paused, not suspended
    void seed();
    void seedClouds();    // (re)build clouds sized relative to the current widget
    void seedScenery();   // (re)build the shore scene's static geometry (cliff, trees, stars)
    void recycleGull(Gull& g);
    void drawGull(QPainter& p, const Gull& g);
    void drawSky(QPainter& p);    // Seagull Morning/Dusk: reactive per-band EQ sky over the shore
    void drawSun(QPainter& p, const QPointF& c, qreal r); // solid gold sun + triangular rays (Morning draws it behind the veiled sky; Day straight on)
    void drawWaves(QPainter& p);  // Seagull Day/Night: the sea scene under a static sky
    void drawShore(QPainter& p, const ScenePalette& pal); // the shared scene: grass/sand/pines/lighthouse + waves
    void drawLighthouse(QPainter& p, qreal night01); // distant point + rocks + tower (+ beam); night01 0..1 fades day->night
    void drawTugboat(QPainter& p, qreal x, qreal y, qreal s, qreal tiltDeg, int dir, qreal night01);
    qreal beamLightAt(qreal x, qreal y) const; // lighthouse beam strength at a point (0 = in the dark)

    // --- Seagull Cycle: one continuous day driven by song progress -------------
    // The four fixed scenes are keyframes; renderCycle walks a time-of-day value
    // tod in [0,1] (position/duration) through them, blending palettes, arcing a
    // single sun off-screen right, rising the moon, and easing night in. The
    // discrete scenes are untouched — this is a parallel render path.
    void renderCycle(QPainter& p, qreal tod);
    void drawCycleSky(QPainter& p, qreal tod);         // unified reactive/plain sky from blended stops
    ScenePalette todPalette(qreal tod) const;          // shore/water colours lerped between keyframes
    struct Seg { Mode a; Mode b; qreal f; };           // the two bounding keyframes + blend 0..1
    Seg   segmentFor(qreal tod) const;                 // schedule: tod -> (a, b, f)
    qreal nightness(qreal tod) const;                  // 0 by day -> 1 deep night (stars/moon/beam)
    qreal reactiveAmt(qreal tod) const;                // music-reactive warmth: peaks Morning/Dusk, low Day/Night
    std::array<QColor, 5> skyStopsFor(Mode m) const;   // the 5 vertical sky stops (synthesised for static modes)
    static QColor lerpColor(const QColor& a, const QColor& b, qreal t);
    void drawClouds(QPainter& p);
    void loadGullFrames();

    Mode m_mode = Mode::Morning;
    bool  m_cycle       = false; // Seagull Cycle: ignore m_mode, run the day arc from the progress below
    qreal m_todProgress = 0.0;   // 0..1 song position/duration TARGET (updated by the 250ms position poll)
    qreal m_todShown    = 0.0;   // eased 0..1 the scene actually renders, so the sun glides between polls
    qreal m_cycleNight  = -1.0;  // scratch nightness for the shared helpers: <0 = not cycling (use m_mode), 0..1 = cycle
    GullBehavior m_behavior = GullBehavior::Drift;
    int  m_maxGulls = 14;          // perf cap

    QVector<QPixmap> m_gullFrames;     // animated gull frames (faces left natively; flipped per direction)
    QVector<QPixmap> m_gullFramesDark; // night-shaded copies: gulls fly dark after sundown,
                                       // lit only where the lighthouse beam catches them
    QVector<int>     m_frameDelays; // per-frame display time (ms) — native gif speed
    int   m_frameIdx = 0;
    qreal m_frameAcc = 0.0;

    QTimer* m_timer = nullptr;
    QVector<Gull>  m_gulls;
    QVector<Cloud> m_clouds;

    // Per-wave 1-D water surfaces (transport fields, advected each step): the
    // heights physically ROLL left-to-right at the tempo-scaled speed. ONE
    // water system: a WAVE GENERATOR just off the left screen edge holds a
    // mound at each band's live level, and advection continuously peels that
    // dance off rightward — every wave on screen is the generator's history.
    // Advection moves the EXISTING water, so a tempo change only alters the
    // flow from now on. [0]=treble, [1]=mid, [2]=bass; cell 0 = the field's
    // off-screen left end.
    static constexpr int kSurfN = 192; // finer grid = less numerical smearing, crests hold shape
    qreal m_surf[3][kSurfN] = {};
    qreal m_waveSpeed = 1.0; // slewed copy of m_tempoSpeed (water pace, no snaps)

    // The occasional tugboat: spawns rarely at one side on a random sheet and
    // chugs across, ALTERNATING direction — never the same crossing twice.
    // He FLOATS on a buoyancy spring (py/vy, lagging and overshooting the
    // water instead of being glued to it); a hard fast riser flings him clear
    // into a gravity arc with a little rebound on splashdown. tilt is his
    // smoothed pitch; lastT gates the physics to one update per animation tick.
    struct Tug { bool active = false; int layer = 0; int dir = 1;
                 qreal x = 0.0, bob = 0.0;
                 qreal py = 0.0, vy = 0.0, tilt = 0.0;
                 qreal lastT = -1.0, smokeT = 0.0; };
    Tug m_tug;
    // His smoke: world-space puffs emitted from the funnel's ACTUAL position
    // each moment (bounces kink the trail), rising and drifting on their own —
    // they hang in the air and finish fading even after he's gone.
    struct Puff { qreal x, y, age, drift; };
    QVector<Puff> m_smoke;

    // Static shore-scene geometry (Waves/Night), rebuilt per resize in seedScenery.
    // The whole scene sits far off at the horizon, drawn BEHIND the waves. Its random
    // layout is drawn from a fixed per-session seed so a resize RESCALES the same
    // scene instead of reshuffling the hills/trees/rocks/stars every time.
    quint32            m_sceneSeed = 0;
    QVector<Star>      m_stars;
    QVector<Tree>      m_trees;
    QPolygonF          m_grass;     // rolling grass line behind the distant sand
    QPolygonF          m_cliff;     // small rocky point the lighthouse stands on (far left)
    QVector<QPolygonF> m_rocks;     // tiny rocks at the waterline by its feet
    QPointF            m_lightBase; // lighthouse base centre on the point's plateau

    // Audio inputs, eased toward their targets (responsive = low latency).
    qreal m_level   = 0.0, m_tLevel   = 0.0;
    qreal m_bass    = 0.0, m_tBass    = 0.0;
    qreal m_mid     = 0.0, m_tMid     = 0.0;
    qreal m_treble  = 0.0, m_tTreble  = 0.0;
    qreal m_colorTone = 0.5; // SLOW-eased spectral balance -> bottom sky hue

    qreal m_t = 0.0; // global clock

    // Tempo: octave-folded beat gaps -> median of the recent window -> eased
    // interval, so one stable glide speed that ignores missed/double-fired kicks.
    qreal m_beatInterval = 0.5; // seconds (~120 BPM default)
    qreal m_lastBeatT    = -1.0;
    qreal m_tempoSpeed   = 1.0; // glide multiplier derived from tempo
    QVector<qreal> m_beatIvals; // recent folded intervals (ring, kTempoWindow wide)
    int   m_beatIvalIdx  = 0;
    // Lighthouse optic: PLAN-view rotation angle (the lens spins in a horizontal
    // plane; the screen shows its projection), its slewed angular rate (starts at
    // the 120 BPM default, glides to the locked tempo — no jarring speed jumps),
    // and the gentle per-beat phase pull that lands the flash ON the beat.
    qreal m_beamA     = 0.0;
    qreal m_beamRate  = 6.2832; // rad/s: half a turn per beat at the 0.5s default interval
    qreal m_beamNudge = 0.0;
    int   m_beamBeats = 1;      // beats per flash: plan advance = pi/m_beamBeats per beat
    qreal m_demoBeatT = 0.0; // demo beat accumulator
    qreal m_spawnT    = 0.0; // spacing for spawning gulls toward the target count
    bool  m_demo      = false;
    bool  m_dying     = false; // EOF death spiral in progress
    bool  m_paused    = false; // playback paused -> freeze the animation
    bool  m_suspended = false; // GUI-thread pressure (e.g. Library build) -> pause rendering
};

#endif // VISUALIZER_H
