#include "hexcaster/eq.h"

#include <algorithm>
#include <cmath>

namespace hexcaster {

// ---------------------------------------------------------------------------
// ProcessorStage interface
// ---------------------------------------------------------------------------

void MidSweepEQ::prepare(float sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;

    // Force coefficient recompute on first block by making cache differ from atomics
    cachedGainDb_  = gainDb_.load(std::memory_order_relaxed) + 1.f;  // != current value
    cachedSweepHz_ = 0.f;
    cachedQ_       = 0.f;

    reset();
}

void MidSweepEQ::reset()
{
    z1_ = 0.f;
    z2_ = 0.f;
}

void MidSweepEQ::process(float* buffer, int numSamples)
{
    // Read atomics once per block
    const float gainDb  = gainDb_.load(std::memory_order_relaxed);
    const float sweepHz = sweepHz_.load(std::memory_order_relaxed);
    const float q       = q_.load(std::memory_order_relaxed);

    // Recompute coefficients only if parameters have changed
    if (gainDb  != cachedGainDb_  ||
        sweepHz != cachedSweepHz_ ||
        q       != cachedQ_)
    {
        cachedGainDb_  = gainDb;
        cachedSweepHz_ = sweepHz;
        cachedQ_       = q;
        updateCoefficients();
    }

    // Biquad Direct Form II Transposed
    // y[n]  = b0*x[n] + z1
    // z1   <- b1*x[n] - a1*y[n] + z2
    // z2   <- b2*x[n] - a2*y[n]
    float z1 = z1_, z2 = z2_;

    for (int i = 0; i < numSamples; ++i) {
        const float x = buffer[i];
        const float y = b0_ * x + z1;
        z1 = b1_ * x - a1_ * y + z2;
        z2 = b2_ * x - a2_ * y;
        buffer[i] = y;
    }

    z1_ = z1;
    z2_ = z2;
}

// ---------------------------------------------------------------------------
// Parameter setters / getters
// ---------------------------------------------------------------------------

void MidSweepEQ::setGainDb (float db) { gainDb_.store (std::clamp(db,   -12.f,  12.f), std::memory_order_relaxed); }
void MidSweepEQ::setSweepHz(float hz) { sweepHz_.store(std::clamp(hz,  300.f, 2500.f), std::memory_order_relaxed); }
void MidSweepEQ::setQ      (float q)  { q_.store      (std::clamp(q,    0.3f,   3.f ), std::memory_order_relaxed); }

float MidSweepEQ::getGainDb()  const { return gainDb_.load (std::memory_order_relaxed); }
float MidSweepEQ::getSweepHz() const { return sweepHz_.load(std::memory_order_relaxed); }
float MidSweepEQ::getQ()       const { return q_.load      (std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Coefficient computation
// Audio EQ Cookbook -- peaking EQ filter
// ---------------------------------------------------------------------------

void MidSweepEQ::updateCoefficients()
{
    // A = amplitude from dB (sqrt form for peaking filter)
    const float A  = std::pow(10.f, cachedGainDb_ / 40.f);

    const float w0    = 2.f * static_cast<float>(M_PI) * cachedSweepHz_ / sampleRate_;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.f * cachedQ_);

    //   b0 =   1 + alpha*A
    //   b1 =  -2*cos(w0)
    //   b2 =   1 - alpha*A
    //   a0 =   1 + alpha/A
    //   a1 =  -2*cos(w0)       (same as b1)
    //   a2 =   1 - alpha/A
    //
    // Normalise by a0 so we store only b0,b1,b2,a1,a2 (a0 = 1 after normalisation).

    const float a0 = 1.f + alpha / A;

    b0_ = (1.f + alpha * A) / a0;
    b1_ = (-2.f * cosw0)    / a0;
    b2_ = (1.f - alpha * A) / a0;
    a1_ = (-2.f * cosw0)    / a0;
    a2_ = (1.f - alpha / A) / a0;
}

} // namespace hexcaster
