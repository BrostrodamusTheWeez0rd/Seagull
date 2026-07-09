#include "SgDynamics.h"

#include <cmath>
#include <algorithm>

namespace {
// One-pole smoothing coefficient for a given time constant (seconds) at a sample rate.
// y += coef * (target - y) reaches ~63% of a step in `seconds`.
inline float onePole(double seconds, int rate) {
    if (seconds <= 0.0 || rate <= 0) return 1.0f;
    return float(1.0 - std::exp(-1.0 / (seconds * double(rate))));
}
inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
inline float linToDb(float lin) { return 20.0f * std::log10(std::max(lin, 1e-9f)); }

// --- Normaliser tuning (tweak by ear) ---------------------------------------
constexpr double kBlockSec     = 0.400; // one loudness measurement block
constexpr float  kDeadZoneDb   = 1.0f;  // the estimate must move more than this before the
                                        // gain target moves at all — kills micro-hunting
constexpr int    kBoostHoldOff = 5;     // gated blocks (~2 s) before we're allowed to boost.
                                        // Attenuation engages from the FIRST block: pulling a
                                        // blasting file down fast is the point, and getting it
                                        // slightly wrong is inaudible next to getting blasted.
                                        // Boosting early off a quiet intro would swell, then
                                        // duck when the track proper arrives — the old bug.
} // namespace

void SgDynamics::prepare(int sampleRate, int channels) {
    m_rate     = sampleRate > 0 ? sampleRate : 44100;
    m_channels = std::max(1, channels);

    m_ceiling    = dbToLin(-1.0f);   // brickwall ceiling
    m_targetLin  = dbToLin(-16.0f);  // normaliser loudness target (RMS)
    m_gateAbs    = dbToLin(-55.0f);  // silence/noise floor: never measured
    m_gateRel    = dbToLin(-10.0f);  // quiet passages: measured out, R128-style
    m_minGain    = dbToLin(-20.0f);  // never pull down more than -20 dB
    m_maxBoost   = dbToLin(12.0f);   // never lift more than +12 dB
    m_blockFrames = std::max(1, int(kBlockSec * m_rate));

    // ~2 ms look-ahead so the limiter's gain is fully down before a transient emerges.
    m_lookahead = std::max(1, int(0.002 * m_rate));
    m_delay.assign(size_t(m_lookahead) * size_t(m_channels), 0.0f);

    // Limiter: very fast attack (≈1 ms, faster than the look-ahead), musical release (~120 ms).
    m_atkCoef = onePole(0.001, m_rate);
    m_relCoef = onePole(0.120, m_rate);

    // Makeup slew. The target it chases is static once the estimate settles, so this only
    // shapes the initial ride onto the track's gain (a -12 dB move is essentially done in
    // ~1.2 s) and softens the rare late correction.
    m_makeupCoef = onePole(0.400, m_rate);

    reset();
}

void SgDynamics::reset() {
    resetTransport();
    // A new track is a new loudness: throw the estimate and the gain away.
    m_sumSq        = 0.0;
    m_nBlocks      = 0;
    m_gainTargetDb = 0.0f;
    m_gainTarget   = 1.0f;
    m_makeup       = 1.0f;
}

void SgDynamics::resetTransport() {
    std::fill(m_delay.begin(), m_delay.end(), 0.0f);
    m_writeIdx   = 0;
    m_limGain    = 1.0f;
    m_blockSumSq = 0.0;   // the partial block spans the discontinuity: drop it
    m_blockFill  = 0;
}

// Fold the just-completed measurement block into the cumulative estimate, subject to
// the two gates, then let the gain target react.
void SgDynamics::finishBlock() {
    const double blockMeanSq = m_blockSumSq / double(m_blockFill);
    m_blockSumSq = 0.0;
    m_blockFill  = 0;

    const float blockRms = float(std::sqrt(std::max(0.0, blockMeanSq)));
    if (blockRms <= m_gateAbs) return;                       // silence / room tone
    if (m_nBlocks > 0) {
        const float integrated = float(std::sqrt(m_sumSq / double(m_nBlocks)));
        if (blockRms < integrated * m_gateRel) return;       // quiet passage
    }
    m_sumSq += blockMeanSq;
    ++m_nBlocks;
    updateGainTarget();
}

void SgDynamics::updateGainTarget() {
    const float integrated = float(std::sqrt(m_sumSq / double(m_nBlocks)));
    if (integrated <= 0.0f) return;

    float gDb = linToDb(m_targetLin / integrated);
    gDb = std::clamp(gDb, linToDb(m_minGain), linToDb(m_maxBoost));
    if (gDb > 0.0f && m_nBlocks < kBoostHoldOff) gDb = 0.0f; // attenuate early, boost late

    // Compare against the TARGET, not the still-sliding makeup, or a move in progress
    // would keep re-triggering and overshoot.
    if (std::fabs(gDb - m_gainTargetDb) < kDeadZoneDb) return;
    m_gainTargetDb = gDb;
    m_gainTarget   = dbToLin(gDb);
}

void SgDynamics::process(float* x, int frames, int channels) {
    const bool on = m_enabled.load(std::memory_order_relaxed);
    if (on != m_wasEnabled) {
        // Toggling clears stale envelope/look-ahead so on->off->on never glitches, and
        // re-measures from scratch (we heard nothing while bypassed).
        reset();
        m_wasEnabled = on;
    }
    if (!on || frames <= 0) return;

    // Re-prepare defensively if the channel count drifted from what we sized for.
    if (channels != m_channels || m_delay.empty()) prepare(m_rate, channels);

    const int ch = m_channels;
    const float ceil = m_ceiling;

    for (int f = 0; f < frames; ++f) {
        float* frame = x + size_t(f) * ch;

        // --- Read frame + measure source loudness (pre-makeup) ---
        float in[8];
        const int nc = std::min(ch, 8);
        float meanSq = 0.0f;
        for (int c = 0; c < nc; ++c) {
            const float s = frame[c]; // already ±1.0 float from VLC's FL32 output
            in[c] = s;
            meanSq += s * s;
        }
        meanSq /= float(nc);

        // --- Normaliser: accumulate the source's loudness in ~400 ms blocks. The gain
        //     target only moves when a completed, gated block shifts the integrated
        //     estimate by more than the dead zone, so mid-track it stops moving entirely
        //     and the music's own dynamics pass through untouched.
        m_blockSumSq += double(meanSq);
        if (++m_blockFill >= m_blockFrames) finishBlock();

        // Slew toward that (settled) target.
        m_makeup += m_makeupCoef * (m_gainTarget - m_makeup);

        // --- Apply makeup, find the frame peak (channel-linked) ---
        float peak = 0.0f;
        for (int c = 0; c < nc; ++c) {
            in[c] *= m_makeup;
            peak = std::max(peak, std::fabs(in[c]));
        }

        // --- Limiter: gain control derived from the (look-ahead) input, applied to the
        //     delayed signal. Fast attack pulls down before the transient exits the delay.
        const float desired = (peak > ceil) ? (ceil / peak) : 1.0f;
        const float coef = (desired < m_limGain) ? m_atkCoef : m_relCoef;
        m_limGain += coef * (desired - m_limGain);

        const int base = m_writeIdx * ch;
        for (int c = 0; c < nc; ++c) {
            const float delayed = m_delay[base + c];
            m_delay[base + c] = in[c];
            float out = delayed * m_limGain;
            // Final hard clamp at the ceiling — the guaranteed brickwall, covering the
            // last sub-millisecond of attack lag on the fastest transients. Output stays
            // float (no 16-bit conversion in our chain), so this is the true ceiling.
            frame[c] = std::clamp(out, -ceil, ceil);
        }
        m_writeIdx = (m_writeIdx + 1) % m_lookahead;
    }
}
