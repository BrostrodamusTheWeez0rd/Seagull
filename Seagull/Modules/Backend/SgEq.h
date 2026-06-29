#ifndef SGEQ_H
#define SGEQ_H

#include <atomic>
#include <mutex>
#include <vector>

// Our own 10-band graphic equalizer for the audio-tap path. We run the EQ ourselves
// (instead of libVLC's built-in equalizer) so the band boosts can be limited downstream
// BEFORE the signal is quantised to S16 — libVLC's EQ runs inside VLC and hard-clips its
// own boost at VLC's S16 output, upstream of anything we can do, and libVLC won't let a
// limiter run after its EQ (it overrides audio-filter). Doing the EQ here, in float on a
// buffer we generated from VLC's clean S16, then limiting (SgDynamics), then quantising
// to S16, means the EQ can never clip. See [[seagull-audio-tap-float-pipeline]].
//
// Bands are RBJ-cookbook peaking (bell) biquads at the same ISO centre frequencies the
// app's UI shows (matching libVLC's 10-band layout), so the user's saved presets — plain
// per-band dB arrays — map across 1:1. At flat (all 0 dB, preamp 0) the chain is exact
// unity passthrough, so toggling the EQ on/off changes tone, not loudness (no insertion
// loss to compensate, unlike libVLC's EQ which needed a +10 dB makeup).
//
// Processes interleaved 32-bit float (±1.0) in place. No Qt deps — it lives on the audio
// output thread's hot pull path and is allocation-free per call after prepare().
class SgEq {
public:
    static constexpr int kBands = 10;

    SgEq() = default;

    // Centre frequency (Hz) of each band — the standard 10-band ISO octave layout,
    // matching libVLC's bands so presets line up. Exposed so the rest of the engine can
    // keep one source of truth if needed.
    static float bandFrequency(int band);
    static int   bandCount() { return kBands; }

    // Allocate per-channel biquad state + compute coefficients for this format. Call
    // before the first process() and whenever rate/channels change.
    void prepare(int sampleRate, int channels);

    // Clear all biquad delay state. Call on (re)start / track boundary so one track's
    // filter ringdown never bleeds into the next.
    void reset();

    // Enable/bypass. Thread-safe (atomic) so the UI thread can toggle while the audio
    // thread processes. Bypass is a true no-op (exact passthrough, no added latency).
    void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Set the per-band gains (dB) and overall preamp (dB). gainsDb must point at `count`
    // values (clamped to kBands). Thread-safe: recomputes coefficients under a short lock
    // so a live slider drag can update while process() runs on the pull thread.
    void setParams(const float* gainsDb, int count, float preampDb);

    // Process `frames` interleaved float samples (channels per frame) in place.
    void process(float* interleaved, int frames, int channels);

private:
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; }; // a0 normalised out

    void recompute(); // (re)build m_co from m_gains/m_preamp/m_rate (call under m_mx)

    std::atomic<bool> m_enabled{ false };

    int   m_rate     = 44100;
    int   m_channels = 2;

    // Parameters + derived coefficients, guarded by m_mx (writer: UI/worker thread;
    // reader: pull thread, which copies them out under the lock once per block).
    std::mutex         m_mx;
    float              m_gains[kBands] = { 0 };
    float              m_preamp = 0.0f;       // dB
    float              m_preampLin = 1.0f;    // linear, derived
    Biquad             m_co[kBands];

    // Per (band, channel) biquad delay state (Direct Form II Transposed). Touched only
    // by process(), so it needs no lock. Sized channels*kBands on prepare().
    std::vector<float> m_z1, m_z2;
};

#endif // SGEQ_H
