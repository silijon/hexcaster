#pragma once

#include "hexcaster/processor_stage.h"

namespace hexcaster {

/**
 * EnvelopeFollower: peak-based envelope detection with configurable
 * attack/release and optional detector pre-filtering.
 *
 * The envelope path is separate from the audio path:
 *   - A high-pass filter (70-150 Hz) is applied to the detection signal only.
 *   - An optional low-pass filter (4-8 kHz) reduces pick-noise spikes.
 *
 * Output is normalized to [0.0, 1.0] and is used by the BloomController
 * to modulate pre-gain and post-gain.
 *
 * NOT a ProcessorStage (does not modify the audio buffer in-place).
 * Called by BloomController prior to the pipeline's stage chain.
 *
 * Implementation: to be added in Phase 1.
 */
class EnvelopeFollower {
public:
    struct Config {
        float attackMs       = 5.f;
        float releaseMs      = 100.f;
        float hpfCutoffHz    = 100.f;
        float lpfCutoffHz    = 6000.f;
        bool  enableLpf      = false;
        int   lookaheadMs    = 0;   // 0 = disabled
    };

    EnvelopeFollower() = default;

    void prepare(float sampleRate, int maxBlockSize, const Config& config);

    /**
     * Analyse a block of audio and return the current envelope value [0,1].
     * Does NOT modify the input buffer.
     * Real-time safe.
     */
    float process(const float* buffer, int numSamples);

    void reset();

    // Not yet implemented
};

} // namespace hexcaster
