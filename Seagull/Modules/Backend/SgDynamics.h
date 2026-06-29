#ifndef SGDYNAMICS_H
#define SGDYNAMICS_H

#include <atomic>
#include <vector>
#include <cstdint>

// Master-bus dynamics for the audio-tap path: a loudness normaliser (slow auto-gain
// that lifts quiet material toward a target) feeding a look-ahead brickwall limiter
// (the hard "never peak" guarantee, ceiling default -1 dBFS). Operates on interleaved
// 32-bit float (±1.0) in place. No Qt deps — it lives on the audio output thread's hot
// pull path, so it stays allocation-free per call after prepare().
//
// IMPORTANT: this works in float but NEVER pulls float from VLC. The pull path reads
// clean S16 from VLC's amem, converts it to a float scratch buffer ourselves, runs our
// own EQ (SgEq) then this stage, and only then quantises back to S16 for the sink.
// Float-from-VLC amem is a confirmed dead end on this setup (it screams) — so all float
// here is on buffers we generated, which is safe. Working in float lets SgEq's band
// boosts overshoot 0 dBFS without being hard-clipped before this limiter can catch them.
//
// Order matters: normalise (drive toward target) -> limit (catch anything over the
// ceiling). The limiter's ceiling is below 0 dBFS, so the S16 conversion that follows
// this stage can never clip even when SgEq's boost or hot source material would
// otherwise overshoot. The user's volume is applied AFTER this (by the QAudioSink),
// so it always wins.
class SgDynamics {
public:
    SgDynamics() = default;

    // Allocate look-ahead buffers + compute time-constant coefficients for this format.
    // Call before the first process() and whenever rate/channels change.
    void prepare(int sampleRate, int channels);

    // Clear envelopes + look-ahead state. Call on (re)start and on a track boundary so
    // one track's gain ride never bleeds into the next.
    void reset();

    // Enable/bypass. Thread-safe (atomic) so the UI thread can toggle while the audio
    // thread processes. Bypass is a true no-op (no added latency).
    void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Process `frames` interleaved float samples (channels per frame) in place.
    void process(float* interleaved, int frames, int channels);

private:
    void resetState();

    std::atomic<bool> m_enabled{ false };
    bool   m_wasEnabled = false;   // edge-detect to reset on the off->on transition

    int    m_rate     = 44100;
    int    m_channels = 2;

    // Limiter ceiling (linear) and look-ahead delay.
    float  m_ceiling  = 0.8913f;   // -1 dBFS
    int    m_lookahead = 0;        // samples
    std::vector<float> m_delay;    // interleaved look-ahead ring (m_lookahead * channels)
    int    m_writeIdx = 0;
    float  m_limGain  = 1.0f;      // smoothed limiter gain
    float  m_atkCoef  = 0.0f;      // limiter attack / release one-pole coefficients
    float  m_relCoef  = 0.0f;

    // Normaliser (loudness leveling) state.
    float  m_targetLin = 0.1585f;  // -16 dBFS RMS target
    float  m_floorLin  = 0.0018f;  // gate: below this RMS we hold gain (don't lift noise)
    float  m_maxBoost  = 4.0f;     // +12 dB ceiling on the makeup
    double m_meanSq    = 0.0;      // running mean-square (one-pole)
    float  m_msCoef    = 0.0f;     // RMS window coefficient
    float  m_makeup    = 1.0f;     // smoothed makeup gain
    float  m_makeupCoef = 0.0f;    // makeup slew coefficient
};

#endif // SGDYNAMICS_H
