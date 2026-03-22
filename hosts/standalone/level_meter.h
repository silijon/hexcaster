#pragma once

#include <atomic>
#include <cmath>

namespace hexcaster {

/**
 * LevelMeter: host-level peak signal meter for TUI display.
 *
 * Intended to measure the signal entering and leaving the DSP pipeline.
 * Called from the audio thread; reads by the TUI thread via atomic.
 *
 * Display range: -60 dB to 0 dB (fixed, not a registry parameter).
 *
 * Usage:
 *   inputMeter.measure(buf, n);   // before pipeline.process()
 *   pipeline.process(buf, n);
 *   outputMeter.measure(buf, n);  // after pipeline.process()
 *
 *   // TUI thread:
 *   float db = inputMeter.getPeakDb();
 *
 * Real-time safety:
 *   measure() is RT-safe: one fabs+max per sample, one atomic store per block.
 *   No allocation, no branching on the hot path beyond the per-sample compare.
 */
class LevelMeter {
public:
    static constexpr float kFloorDb    = -60.f;
    static constexpr float kCeilingDb  =   0.f;

    /**
     * Scan the block for peak absolute amplitude, convert to dB,
     * and publish via the observation atomic.
     * Called from the audio thread.
     */
    void measure(const float* buf, int numSamples)
    {
        float peak = 0.f;
        for (int i = 0; i < numSamples; ++i) {
            const float s = buf[i] < 0.f ? -buf[i] : buf[i];
            if (s > peak) peak = s;
        }

        float db;
        if (peak <= 0.f) {
            db = kFloorDb;
        } else {
            db = 20.f * std::log10f(peak);
            if (db < kFloorDb)    db = kFloorDb;
            if (db > kCeilingDb)  db = kCeilingDb;
        }

        observedPeakDb_.store(db, std::memory_order_relaxed);
    }

    /**
     * Read the most recently measured peak level in dB.
     * Safe to call from any thread (relaxed atomic load).
     */
    float getPeakDb() const
    {
        return observedPeakDb_.load(std::memory_order_relaxed);
    }

    /**
     * Normalise a dB value to [0, 1] for gauge display.
     * Maps [kFloorDb, kCeilingDb] linearly to [0, 1].
     */
    static float normalise(float db)
    {
        const float span = kCeilingDb - kFloorDb;
        if (span <= 0.f) return 0.f;
        float n = (db - kFloorDb) / span;
        if (n < 0.f) n = 0.f;
        if (n > 1.f) n = 1.f;
        return n;
    }

private:
    std::atomic<float> observedPeakDb_{ kFloorDb };
};

} // namespace hexcaster
