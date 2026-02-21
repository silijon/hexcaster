#pragma once

#include "hexcaster/processor_stage.h"

namespace hexcaster {

/**
 * ParametricEQ: biquad-based parametric equalizer.
 *
 * Two roles in the signal chain:
 *   1. Pre-distortion EQ (input shaping before NAM): HPF + optional low-shelf.
 *   2. Post-distortion EQ (tone control after NAM): 3-5 band parametric.
 *
 * Implementation details:
 *   - Biquad Direct Form II Transposed
 *   - Coefficients pre-computed in prepare() and on parameter change
 *   - Smoothed coefficient transitions to avoid zipper noise
 *   - Stable at extreme parameter values
 *   - No dynamic allocation
 *
 * Implementation: to be added in Phase 1.
 */
class ParametricEQ : public ProcessorStage {
public:
    ParametricEQ() = default;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    // Band configuration -- to be expanded
};

} // namespace hexcaster
