#pragma once

#include "hexcaster/processor_stage.h"

#include <atomic>
#include <cstdint>

namespace hexcaster {

/**
 * NoiseGate: silence the signal when its level drops below a threshold.
 *
 * Tracks a per-sample peak envelope and applies a gate gain (0.0–1.0)
 * to each sample. The gate gain transitions smoothly between open and
 * closed to avoid clicks.
 *
 * State machine:
 *   CLOSED  -- gate gain ramps toward 0. Signal is muted.
 *   OPENING -- signal exceeded threshold; gain ramps toward 1.
 *   OPEN    -- gate gain is at 1. Signal passes through.
 *   HOLDING -- signal dropped below threshold; hold counter running.
 *              Gate stays open until hold expires, then enters CLOSING.
 *   CLOSING -- hold expired; gain ramps toward 0.
 *
 * Parameters (all settable from the control thread, atomic):
 *   threshold_dB  -- gate opens above this level  [-80, 0] dB, default -60
 *   attackMs      -- time to fully open            [0.1, 10] ms, default 0.5
 *   releaseMs     -- time to fully close           [5, 500] ms, default 50
 *   holdMs        -- min open time after drop      [0, 500] ms, default 50
 *
 * Real-time safety:
 *   process() is RT-safe: no allocation, no branching on hot path beyond
 *   the state machine enum comparison.
 *   Coefficients are recomputed at the top of each block from the atomics,
 *   not per-sample, so parameter changes take effect within one block (~10ms).
 */
class NoiseGate : public ProcessorStage {
public:
    NoiseGate() = default;

    /** Gate state, exposed publicly for TUI observation. */
    enum class State : uint8_t { Closed, Opening, Open, Holding, Closing };

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    // Control thread setters (atomic, safe to call any time)
    void setThresholdDb(float db);
    void setAttackMs(float ms);
    void setReleaseMs(float ms);
    void setHoldMs(float ms);

    float getThresholdDb()  const;
    float getAttackMs()     const;
    float getReleaseMs()    const;
    float getHoldMs()       const;

    /**
     * Read the current gate gain [0.0 = fully closed, 1.0 = fully open].
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block. Intended for TUI metering only.
     */
    float getGateGain() const;

    /**
     * Read the current gate state.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block. Intended for TUI metering only.
     */
    State getState() const;

private:

    // Recompute coefficients from current atomic values.
    // Called at the top of each block -- not RT-unsafe, just a few maths ops.
    void updateCoefficients();

    static float msToCoeff(float ms, float sampleRate);
    static float dbToLinear(float db);

    // --- Observation atomics (written by audio thread, read by TUI thread) ---
    // Updated once per block at the end of process(). Relaxed ordering.
    std::atomic<float>   observedGateGain_{ 0.f };
    std::atomic<uint8_t> observedState_{ static_cast<uint8_t>(State::Closed) };

    // --- Atomic parameters (written by control thread) ---
    std::atomic<float> thresholdDb_{ -60.f };
    std::atomic<float> attackMs_{    0.5f  };
    std::atomic<float> releaseMs_{  50.f   };
    std::atomic<float> holdMs_{     50.f   };

    // --- Audio thread state (only touched in process()) ---
    float sampleRate_     = 48000.f;
    State state_          = State::Closed;
    float envelope_       = 0.f;   // current peak envelope (linear)
    float gateGain_       = 0.f;   // current applied gain (0 = closed, 1 = open)
    int   holdCounter_    = 0;     // samples remaining in hold

    // Derived from atomics each block
    float thresholdLin_   = 0.f;
    float attackCoeff_    = 0.f;   // EMA coeff for opening (close to 0 = fast)
    float releaseCoeff_   = 0.f;   // EMA coeff for closing
    float envReleaseCoeff_= 0.f;   // EMA coeff for envelope follower decay
    int   holdSamples_    = 0;
};

} // namespace hexcaster
