#pragma once

namespace hexcaster {

/**
 * Abstract interface for all DSP processing stages.
 *
 * Rules:
 * - prepare() is called once at initialization (not real-time safe).
 * - process() must be real-time safe: no allocation, no blocking, no I/O.
 * - reset() clears internal state (filters, buffers) without reallocating.
 *
 * Stages are single-channel (mono). The pipeline manages channel routing.
 */
class ProcessorStage {
public:
    virtual ~ProcessorStage() = default;

    /**
     * Called before the audio thread starts.
     * Allocate buffers, compute coefficients, etc.
     *
     * @param sampleRate    Audio sample rate in Hz.
     * @param maxBlockSize  Maximum number of samples per process() call.
     */
    virtual void prepare(float sampleRate, int maxBlockSize) = 0;

    /**
     * Process a block of audio in-place.
     * Real-time safe. Must complete in bounded time.
     *
     * @param buffer        Pointer to interleaved float samples (in-place).
     * @param numSamples    Number of samples to process (<= maxBlockSize).
     */
    virtual void process(float* buffer, int numSamples) = 0;

    /**
     * Reset internal state (filter memories, envelope state, etc.)
     * without reallocating buffers.
     * Real-time safe.
     */
    virtual void reset() = 0;
};

} // namespace hexcaster
