#pragma once

#include "hexcaster/processor_stage.h"

namespace hexcaster {

/**
 * IRConvolver: partitioned convolution for cabinet impulse response simulation.
 *
 * Used only in Direct Mode (headphones / recording).
 * Not present in the Cab Mode pipeline.
 *
 * Implementation:
 *   - Uniform partitioned convolution (OLS/OLA)
 *   - Fixed maximum IR length (set at prepare() time)
 *   - All buffers pre-allocated, no dynamic allocation in process()
 *   - IR loaded at initialization only, never during processing
 *
 * Implementation: to be added in Phase 1.
 */
class IRConvolver : public ProcessorStage {
public:
    static constexpr int kMaxIRLengthSamples = 48000; // 1 second @ 48kHz

    IRConvolver() = default;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    /**
     * Load an impulse response. Not real-time safe.
     * Must be called before the audio thread starts, or from a non-RT thread.
     *
     * @param ir         Pointer to IR samples.
     * @param irLength   Number of samples in the IR (<= kMaxIRLengthSamples).
     * @return           true on success, false if irLength exceeds maximum.
     */
    bool loadIR(const float* ir, int irLength);
};

} // namespace hexcaster
