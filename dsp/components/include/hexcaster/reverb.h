#pragma once

#include "hexcaster/processor_stage.h"

namespace hexcaster {

/**
 * Reverb: lightweight algorithmic reverb (Freeverb-style).
 *
 * Used only in Direct Mode, after IR convolution.
 *
 * Requirements:
 *   - CPU efficient
 *   - Deterministic
 *   - No dynamic memory
 *   - Parameters: room size, damping, wet/dry mix
 *
 * Implementation: to be added in Phase 3.
 */
class Reverb : public ProcessorStage {
public:
    Reverb() = default;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    void setRoomSize(float roomSize);   // [0.0, 1.0]
    void setDamping(float damping);     // [0.0, 1.0]
    void setWetDry(float wet);          // [0.0, 1.0]
};

} // namespace hexcaster
