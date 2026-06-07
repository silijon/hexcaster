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

// ===========================================================================
// ShelfEQ -- high-shelf + low-shelf cascaded biquads
// ===========================================================================

void ShelfEQ::prepare(float sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;

    // Force coefficient recompute on the first block by making caches differ
    // from the current atomic values.
    cachedHighHz_   = 0.f;
    cachedHighGain_ = highGain_.load(std::memory_order_relaxed) + 1.f;  // != current
    cachedHighBw_   = 0.f;
    cachedLowHz_    = 0.f;
    cachedLowGain_  = lowGain_.load(std::memory_order_relaxed) + 1.f;   // != current
    cachedLowBw_    = 0.f;

    reset();
}

void ShelfEQ::reset()
{
    hz1_ = hz2_ = 0.f;
    lz1_ = lz2_ = 0.f;
}

void ShelfEQ::process(float* buffer, int numSamples)
{
    // Read atomics once per block
    const float highHz   = highHz_.load  (std::memory_order_relaxed);
    const float highGain = highGain_.load(std::memory_order_relaxed);
    const float highBw   = highBw_.load  (std::memory_order_relaxed);
    const float lowHz    = lowHz_.load   (std::memory_order_relaxed);
    const float lowGain  = lowGain_.load (std::memory_order_relaxed);
    const float lowBw    = lowBw_.load   (std::memory_order_relaxed);

    // Recompute each shelf's coefficients only if its parameters changed
    if (highHz != cachedHighHz_ || highGain != cachedHighGain_ || highBw != cachedHighBw_) {
        cachedHighHz_ = highHz; cachedHighGain_ = highGain; cachedHighBw_ = highBw;
        updateHighShelf();
    }
    if (lowHz != cachedLowHz_ || lowGain != cachedLowGain_ || lowBw != cachedLowBw_) {
        cachedLowHz_ = lowHz; cachedLowGain_ = lowGain; cachedLowBw_ = lowBw;
        updateLowShelf();
    }

    // Cascade: high-shelf biquad, then low-shelf biquad. Both DF2T:
    //   y[n]  = b0*x[n] + z1
    //   z1   <- b1*x[n] - a1*y[n] + z2
    //   z2   <- b2*x[n] - a2*y[n]
    float hz1 = hz1_, hz2 = hz2_;
    float lz1 = lz1_, lz2 = lz2_;

    for (int i = 0; i < numSamples; ++i) {
        const float x = buffer[i];

        const float yh = hb0_ * x + hz1;
        hz1 = hb1_ * x - ha1_ * yh + hz2;
        hz2 = hb2_ * x - ha2_ * yh;

        const float yl = lb0_ * yh + lz1;
        lz1 = lb1_ * yh - la1_ * yl + lz2;
        lz2 = lb2_ * yh - la2_ * yl;

        buffer[i] = yl;
    }

    hz1_ = hz1; hz2_ = hz2;
    lz1_ = lz1; lz2_ = lz2;
}

// ---------------------------------------------------------------------------
// Parameter setters / getters
// ---------------------------------------------------------------------------

void ShelfEQ::setHighShelfHz    (float hz)  { highHz_.store  (std::clamp(hz,  1000.f, 16000.f), std::memory_order_relaxed); }
void ShelfEQ::setHighShelfGainDb(float db)  { highGain_.store(std::clamp(db,   -32.f,    12.f), std::memory_order_relaxed); }
void ShelfEQ::setHighShelfBw    (float oct) { highBw_.store  (std::clamp(oct,   0.1f,     4.f), std::memory_order_relaxed); }
void ShelfEQ::setLowShelfHz     (float hz)  { lowHz_.store   (std::clamp(hz,    40.f,  1000.f), std::memory_order_relaxed); }
void ShelfEQ::setLowShelfGainDb (float db)  { lowGain_.store (std::clamp(db,   -32.f,    12.f), std::memory_order_relaxed); }
void ShelfEQ::setLowShelfBw     (float oct) { lowBw_.store   (std::clamp(oct,   0.1f,     4.f), std::memory_order_relaxed); }

float ShelfEQ::getHighShelfHz()     const { return highHz_.load  (std::memory_order_relaxed); }
float ShelfEQ::getHighShelfGainDb() const { return highGain_.load(std::memory_order_relaxed); }
float ShelfEQ::getHighShelfBw()     const { return highBw_.load  (std::memory_order_relaxed); }
float ShelfEQ::getLowShelfHz()      const { return lowHz_.load   (std::memory_order_relaxed); }
float ShelfEQ::getLowShelfGainDb()  const { return lowGain_.load (std::memory_order_relaxed); }
float ShelfEQ::getLowShelfBw()      const { return lowBw_.load   (std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Coefficient computation
// Audio EQ Cookbook -- shelving filters, bandwidth (octaves) form for alpha:
//   A     = 10^(gainDb/40)
//   w0    = 2*pi*f0/Fs
//   alpha = sin(w0) * sinh( ln(2)/2 * BW * w0/sin(w0) )
//   beta  = 2*sqrt(A)*alpha
// Coefficients are normalised by a0 so we store only b0,b1,b2,a1,a2.
// ---------------------------------------------------------------------------

namespace {
constexpr float kLn2 = 0.69314718056f;
}

void ShelfEQ::updateHighShelf()
{
    const float A     = std::pow(10.f, cachedHighGain_ / 40.f);
    const float sqrtA = std::sqrt(A);
    const float w0    = 2.f * static_cast<float>(M_PI) * cachedHighHz_ / sampleRate_;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 * std::sinh(0.5f * kLn2 * cachedHighBw_ * w0 / sinw0);
    const float beta  = 2.f * sqrtA * alpha;

    const float b0 =        A * ((A + 1.f) + (A - 1.f) * cosw0 + beta);
    const float b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cosw0);
    const float b2 =        A * ((A + 1.f) + (A - 1.f) * cosw0 - beta);
    const float a0 =            ((A + 1.f) - (A - 1.f) * cosw0 + beta);
    const float a1 =  2.f *     ((A - 1.f) - (A + 1.f) * cosw0);
    const float a2 =            ((A + 1.f) - (A - 1.f) * cosw0 - beta);

    hb0_ = b0 / a0; hb1_ = b1 / a0; hb2_ = b2 / a0;
    ha1_ = a1 / a0; ha2_ = a2 / a0;
}

void ShelfEQ::updateLowShelf()
{
    const float A     = std::pow(10.f, cachedLowGain_ / 40.f);
    const float sqrtA = std::sqrt(A);
    const float w0    = 2.f * static_cast<float>(M_PI) * cachedLowHz_ / sampleRate_;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 * std::sinh(0.5f * kLn2 * cachedLowBw_ * w0 / sinw0);
    const float beta  = 2.f * sqrtA * alpha;

    const float b0 =        A * ((A + 1.f) - (A - 1.f) * cosw0 + beta);
    const float b1 =  2.f * A * ((A - 1.f) - (A + 1.f) * cosw0);
    const float b2 =        A * ((A + 1.f) - (A - 1.f) * cosw0 - beta);
    const float a0 =            ((A + 1.f) + (A - 1.f) * cosw0 + beta);
    const float a1 = -2.f *     ((A - 1.f) + (A + 1.f) * cosw0);
    const float a2 =            ((A + 1.f) + (A - 1.f) * cosw0 - beta);

    lb0_ = b0 / a0; lb1_ = b1 / a0; lb2_ = b2 / a0;
    la1_ = a1 / a0; la2_ = a2 / a0;
}

} // namespace hexcaster
