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

} // namespace hexcaster
