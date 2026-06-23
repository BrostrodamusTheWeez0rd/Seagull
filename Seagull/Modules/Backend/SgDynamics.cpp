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
} // namespace

void SgDynamics::prepare(int sampleRate, int channels) {
    m_rate     = sampleRate > 0 ? sampleRate : 44100;
    m_channels = std::max(1, channels);

    m_ceiling    = dbToLin(-1.0f);   // brickwall ceiling
    m_targetLin  = dbToLin(-16.0f);  // normaliser loudness target (RMS)
    m_floorLin   = dbToLin(-55.0f);  // below this we hold the makeup (silence/noise floor)
    m_maxBoost   = dbToLin(12.0f);   // never lift more than +12 dB

    // ~2 ms look-ahead so the limiter's gain is fully down before a transient emerges.
    m_lookahead = std::max(1, int(0.002 * m_rate));
    m_delay.assign(size_t(m_lookahead) * size_t(m_channels), 0.0f);

    // Limiter: very fast attack (≈1 ms, faster than the look-ahead), musical release (~120 ms).
    m_atkCoef = onePole(0.001, m_rate);
    m_relCoef = onePole(0.120, m_rate);

    // Normaliser: ~300 ms RMS window, ~400 ms makeup slew — slow enough not to pump.
    m_msCoef     = onePole(0.300, m_rate);
    m_makeupCoef = onePole(0.400, m_rate);

    resetState();
}

void SgDynamics::reset() { resetState(); }

void SgDynamics::resetState() {
    std::fill(m_delay.begin(), m_delay.end(), 0.0f);
    m_writeIdx = 0;
    m_limGain  = 1.0f;
    m_meanSq   = 0.0;
    m_makeup   = 1.0f;
}

void SgDynamics::process(int16_t* x, int frames, int channels) {
    const bool on = m_enabled.load(std::memory_order_relaxed);
    if (on != m_wasEnabled) {
        // Toggling clears stale envelope/look-ahead so on->off->on never glitches.
        resetState();
        m_wasEnabled = on;
    }
    if (!on || frames <= 0) return;

    // Re-prepare defensively if the channel count drifted from what we sized for.
    if (channels != m_channels || m_delay.empty()) prepare(m_rate, channels);

    const int ch = m_channels;
    const float ceil = m_ceiling;

    for (int f = 0; f < frames; ++f) {
        int16_t* frame = x + size_t(f) * ch;

        // --- Read frame to float + measure source loudness (pre-makeup) ---
        float in[8];
        const int nc = std::min(ch, 8);
        float meanSq = 0.0f;
        for (int c = 0; c < nc; ++c) {
            const float s = frame[c] * (1.0f / 32768.0f);
            in[c] = s;
            meanSq += s * s;
        }
        meanSq /= float(nc);

        // --- Normaliser: slew a makeup gain toward (target / current RMS) ---
        m_meanSq += double(m_msCoef) * (double(meanSq) - m_meanSq);
        const float rms = float(std::sqrt(std::max(0.0, m_meanSq)));
        float makeupTarget = m_makeup;
        if (rms > m_floorLin)
            makeupTarget = std::clamp(m_targetLin / rms, 1.0f, m_maxBoost); // boost-only
        m_makeup += m_makeupCoef * (makeupTarget - m_makeup);

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
            // last sub-millisecond of attack lag on the fastest transients.
            out = std::clamp(out, -ceil, ceil);
            int v = int(std::lround(out * 32768.0f));
            frame[c] = int16_t(std::clamp(v, -32768, 32767));
        }
        m_writeIdx = (m_writeIdx + 1) % m_lookahead;
    }
}
