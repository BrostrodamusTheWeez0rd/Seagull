#include "SgEq.h"

#include <cmath>
#include <algorithm>

namespace {
// Standard 10-band ISO octave centres, matching libVLC's 10-band equalizer so the user's
// saved per-band gain presets map across 1:1.
constexpr float kFreq[SgEq::kBands] = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};
// One octave between adjacent bands -> bandwidth ~1 octave -> Q = sqrt(2^N)/(2^N-1) with
// N=1 => ~1.414. A common, musical graphic-EQ default; tune by ear if the bells feel too
// wide/narrow (lower Q = wider, more overlap; higher Q = tighter, more surgical).
constexpr float kQ = 1.414f;
constexpr float kPi = 3.14159265358979323846f;

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
} // namespace

float SgEq::bandFrequency(int band) {
    if (band < 0 || band >= kBands) return 0.0f;
    return kFreq[band];
}

void SgEq::prepare(int sampleRate, int channels) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_rate     = sampleRate > 0 ? sampleRate : 44100;
    m_channels = channels  > 0 ? channels  : 2;
    m_z1.assign(size_t(m_channels) * kBands, 0.0f);
    m_z2.assign(size_t(m_channels) * kBands, 0.0f);
    recompute();
}

void SgEq::reset() {
    std::fill(m_z1.begin(), m_z1.end(), 0.0f);
    std::fill(m_z2.begin(), m_z2.end(), 0.0f);
}

void SgEq::setParams(const float* gainsDb, int count, float preampDb) {
    std::lock_guard<std::mutex> lk(m_mx);
    const int n = std::min(count, kBands);
    for (int i = 0; i < n; ++i)      m_gains[i] = gainsDb[i];
    for (int i = n; i < kBands; ++i) m_gains[i] = 0.0f;
    m_preamp = preampDb;
    recompute();
}

void SgEq::recompute() {
    // Called under m_mx. RBJ "Audio EQ Cookbook" peaking-EQ biquad per band.
    m_preampLin = dbToLin(m_preamp);
    for (int b = 0; b < kBands; ++b) {
        const float A     = std::pow(10.0f, m_gains[b] / 40.0f); // sqrt of linear gain
        const float w0    = 2.0f * kPi * kFreq[b] / float(m_rate);
        const float cosw0 = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * kQ);

        const float b0 =  1.0f + alpha * A;
        const float b1 = -2.0f * cosw0;
        const float b2 =  1.0f - alpha * A;
        const float a0 =  1.0f + alpha / A;
        const float a1 = -2.0f * cosw0;
        const float a2 =  1.0f - alpha / A;

        const float inv = 1.0f / a0;
        m_co[b].b0 = b0 * inv;
        m_co[b].b1 = b1 * inv;
        m_co[b].b2 = b2 * inv;
        m_co[b].a1 = a1 * inv;
        m_co[b].a2 = a2 * inv;
    }
}

void SgEq::process(float* x, int frames, int channels) {
    if (!m_enabled.load(std::memory_order_relaxed)) return; // exact passthrough when off
    if (frames <= 0 || channels <= 0) return;

    // Snapshot coefficients under the lock once per block, then run the (long) sample loop
    // lock-free. A live slider drag only ever delays a coefficient change by one block.
    Biquad co[kBands];
    float preamp;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        std::copy(std::begin(m_co), std::end(m_co), co);
        preamp = m_preampLin;
    }

    // If the format changed under us (channels mismatch vs prepared state), bail rather
    // than index past the delay buffers.
    if (channels > m_channels) return;

    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            float s = x[f * channels + c] * preamp;
            // Cascade the 10 peaking biquads. Direct Form II Transposed: stable, one
            // mul-add pair of state per band, no separate input history needed.
            for (int b = 0; b < kBands; ++b) {
                const size_t st = size_t(c) * kBands + b;
                const float y = co[b].b0 * s + m_z1[st];
                m_z1[st] = co[b].b1 * s - co[b].a1 * y + m_z2[st];
                m_z2[st] = co[b].b2 * s - co[b].a2 * y;
                s = y;
            }
            x[f * channels + c] = s;
        }
    }
}
