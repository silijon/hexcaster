#pragma once

#include "hexcaster/processor_stage.h"

#include <atomic>

namespace hexcaster {

/**
 * MidSweepEQ: single-band biquad peaking (bell) filter for post-NAM tone shaping.
 *
 * A classic amp mid-control: one knob sweeps the center frequency across the
 * midrange, another boosts or cuts at that frequency.
 *
 * Parameters:
 *   gainDb   -- boost/cut in dB at the center frequency.  [-12, +12], default 0
 *   sweepHz  -- center frequency of the bell filter.       [300, 2500] Hz, default 1000
 *   q        -- bandwidth (Q factor). Higher Q = narrower bell. [0.3, 3.0], default 0.8
 *
 * Implementation:
 *   Biquad Direct Form II Transposed (DF2T) -- numerically stable, two delay elements.
 *   Coefficients use the Audio EQ Cookbook peaking filter formulas.
 *   Coefficients are recomputed once per block only when a parameter has changed,
 *   avoiding redundant transcendental function calls during steady-state playback.
 *   At 0 dB gain the filter collapses to unity (A=1, alpha terms cancel) -- no
 *   separate bypass path needed.
 *
 * Real-time safety:
 *   process() is RT-safe: no allocation, no I/O.
 *   Parameter atomics are read at the top of each block, not per-sample.
 */
class MidSweepEQ : public ProcessorStage {
public:
    MidSweepEQ() = default;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    // Control thread setters (atomic -- safe to call any time)
    void setGainDb (float db);   // clamped to [-12, +12]
    void setSweepHz(float hz);   // clamped to [300, 2500]
    void setQ      (float q);    // clamped to [0.3, 3.0]

    float getGainDb()  const;
    float getSweepHz() const;
    float getQ()       const;

private:
    void updateCoefficients();

    // --- Atomic parameters (control thread) ---
    std::atomic<float> gainDb_ {  0.f   };
    std::atomic<float> sweepHz_{ 1000.f };
    std::atomic<float> q_      {  0.8f  };

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // Biquad coefficients (normalised, a0 = 1)
    float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f;
    float      a1_ = 0.f, a2_ = 0.f;

    // DF2T delay elements
    float z1_ = 0.f, z2_ = 0.f;

    // Cached param values for change detection (avoid redundant coefficient recompute)
    float cachedGainDb_  = 0.f;
    float cachedSweepHz_ = 0.f;
    float cachedQ_       = 0.f;
};

/**
 * ShelfEQ: combined high-shelf + low-shelf tone control (two cascaded biquads).
 *
 * Post-NAM voicing: a low shelf lifts or cuts the body, a high shelf tames or
 * brightens the top end. Each shelf is an independent Audio EQ Cookbook shelving
 * biquad (DF2T), cascaded in series within one ProcessorStage.
 *
 * Parameters (default / range):
 *   highShelfHz     3000 Hz   [1000, 16000]   high-shelf corner frequency
 *   highShelfGainDb 0.0 dB    [-32, +12]      high-shelf boost/cut
 *   highShelfBw     1.5 oct   [0.1, 4.0]      high-shelf bandwidth (octaves)
 *   lowShelfHz      175 Hz    [40, 1000]      low-shelf corner frequency
 *   lowShelfGainDb  0.0 dB    [-32, +12]      low-shelf boost/cut
 *   lowShelfBw      1.5 oct   [0.1, 4.0]      low-shelf bandwidth (octaves)
 *
 * Implementation mirrors MidSweepEQ: coefficients are recomputed once per block
 * only when a parameter has changed. At 0 dB gain each shelf collapses to unity
 * (A=1) -- no bypass path needed.
 *
 * Real-time safety:
 *   process() is RT-safe: no allocation, no I/O.
 *   Parameter atomics are read at the top of each block, not per-sample.
 */
class ShelfEQ : public ProcessorStage {
public:
    ShelfEQ() = default;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    // Control thread setters (atomic -- safe to call any time)
    void setHighShelfHz    (float hz);   // clamped to [1000, 16000]
    void setHighShelfGainDb(float db);   // clamped to [-32, +12]
    void setHighShelfBw    (float oct);  // clamped to [0.1, 4.0]
    void setLowShelfHz     (float hz);   // clamped to [40, 1000]
    void setLowShelfGainDb (float db);   // clamped to [-32, +12]
    void setLowShelfBw     (float oct);  // clamped to [0.1, 4.0]

    float getHighShelfHz()     const;
    float getHighShelfGainDb() const;
    float getHighShelfBw()     const;
    float getLowShelfHz()      const;
    float getLowShelfGainDb()  const;
    float getLowShelfBw()      const;

private:
    void updateHighShelf();
    void updateLowShelf();

    // --- Atomic parameters (control thread) ---
    std::atomic<float> highHz_  { 3000.f };
    std::atomic<float> highGain_{ 0.f    };
    std::atomic<float> highBw_  { 1.5f   };
    std::atomic<float> lowHz_   {  175.f };
    std::atomic<float> lowGain_ {  0.f   };
    std::atomic<float> lowBw_   {  1.5f  };

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // High-shelf biquad (normalised, a0 = 1) + DF2T delay elements
    float hb0_ = 1.f, hb1_ = 0.f, hb2_ = 0.f;
    float        ha1_ = 0.f, ha2_ = 0.f;
    float hz1_ = 0.f, hz2_ = 0.f;

    // Low-shelf biquad (normalised, a0 = 1) + DF2T delay elements
    float lb0_ = 1.f, lb1_ = 0.f, lb2_ = 0.f;
    float        la1_ = 0.f, la2_ = 0.f;
    float lz1_ = 0.f, lz2_ = 0.f;

    // Cached param values for change detection
    float cachedHighHz_ = 0.f, cachedHighGain_ = 0.f, cachedHighBw_ = 0.f;
    float cachedLowHz_  = 0.f, cachedLowGain_  = 0.f, cachedLowBw_  = 0.f;
};

} // namespace hexcaster
