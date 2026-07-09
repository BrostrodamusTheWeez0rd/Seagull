#ifndef SGDYNAMICS_H
#define SGDYNAMICS_H

#include <atomic>
#include <vector>
#include <cstdint>

// Master-bus dynamics for the audio-tap path: a loudness normaliser feeding a look-ahead
// brickwall limiter (the hard "never peak" guarantee, ceiling default -1 dBFS).
//
// The normaliser is a LEVELLER, not a compressor. It measures the programme's integrated
// loudness and applies ONE static gain per file — quiet files up, blasting files down —
// so files match each other while every file keeps its own internal dynamics. It must
// never be audible as a gain move once a track is under way; if you can hear it working
// on the music, it is broken. The limiter, downstream, is what catches peaks.
//
// Operates on interleaved
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

    // Full clear, INCLUDING the integrated loudness estimate. Call only on a real track
    // boundary — a new track is a new loudness, so the estimate must be rebuilt.
    void reset();

    // Clear only the transport-local state: the look-ahead ring, the limiter envelope
    // and the partial measurement block. The integrated loudness estimate and the gain
    // it settled on SURVIVE. Call this on a seek or a FIFO underrun — those are the same
    // audio, so re-measuring it would dump the gain back to unity and audibly ride it
    // back up, which is exactly the artefact this class exists to avoid.
    void resetTransport();

    // Enable/bypass. Thread-safe (atomic) so the UI thread can toggle while the audio
    // thread processes. Bypass is a true no-op (no added latency).
    void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Process `frames` interleaved float samples (channels per frame) in place.
    void process(float* interleaved, int frames, int channels);

private:
    void finishBlock();     // fold one measurement block into the integrated estimate
    void updateGainTarget();

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

    // --- Normaliser (loudness levelling) ---------------------------------------
    // The gain is NOT recomputed continuously from a short window — that is a slow
    // compressor, and it both pumps and flattens the music. Instead we integrate the
    // programme's loudness over gated blocks, cumulatively, for as long as the track
    // plays. A cumulative mean is self-stabilising: it moves freely over the first
    // seconds, then each new block shifts it less and less, so the gain converges to
    // one value and stays there. That is what "same volume between files, dynamics
    // intact" actually requires — a single static offset per file.
    float  m_targetLin = 0.1585f;  // -16 dBFS target loudness
    float  m_gateAbs   = 0.0018f;  // absolute gate (-55 dBFS): silence/noise never counts
    float  m_gateRel   = 0.3162f;  // relative gate (-10 dB under the estimate): quiet
                                   // passages don't drag the estimate down and provoke a
                                   // boost that the next loud passage has to undo
    float  m_minGain   = 0.1f;     // -20 dB: how far we may pull a blasting file DOWN
    float  m_maxBoost  = 4.0f;     // +12 dB: how far we may lift a quiet file UP

    // Measurement block accumulator (~400 ms).
    int    m_blockFrames = 0;
    int    m_blockFill   = 0;
    double m_blockSumSq  = 0.0;

    // Integrated estimate: cumulative mean-square over gated blocks.
    double m_sumSq   = 0.0;
    int    m_nBlocks = 0;

    // Gain: a target that only moves when the estimate meaningfully changes, and a
    // one-pole slew toward it. With a settled target the slew is silent; it was the
    // ever-moving target, not the slew, that made the old version audible.
    float  m_gainTargetDb = 0.0f;
    float  m_gainTarget   = 1.0f;
    float  m_makeup       = 1.0f;  // smoothed makeup gain actually applied
    float  m_makeupCoef   = 0.0f;  // makeup slew coefficient
};

#endif // SGDYNAMICS_H
