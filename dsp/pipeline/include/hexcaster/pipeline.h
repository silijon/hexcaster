#pragma once

#include <array>
#include <cstdint>
#include "hexcaster/processor_stage.h"

namespace hexcaster {

/**
 * PipelineController: cross-cutting observer/modifier for the pipeline.
 *
 * Controllers are called at defined points in the stage chain, allowing
 * them to observe the signal and inject modifications (e.g., gain values)
 * that straddle multiple stages.
 *
 * The primary use case is BloomController, which:
 *   1. Observes the input buffer to update the envelope follower.
 *   2. Injects pre-gain before the amp model stage.
 *   3. Injects post-gain after the amp model stage.
 *
 * Controllers are lightweight: they do NOT own stages, they reference them.
 * Real-time safe: no allocation, no blocking.
 */
class PipelineController {
public:
    virtual ~PipelineController() = default;

    /**
     * Called once before any stages run, with the original input buffer.
     * Use this to update internal state (e.g., run the envelope follower).
     * Must NOT modify the buffer.
     *
     * Real-time safe.
     */
    virtual void preProcess(const float* buffer, int numSamples) = 0;

    /**
     * Called after stage[stageIndex] has run, before stage[stageIndex+1].
     * Use this to modify the buffer between specific stages.
     *
     * Real-time safe.
     *
     * @param stageIndex  Index of the stage that just completed.
     * @param buffer      The audio buffer (may be modified in-place).
     * @param numSamples  Number of samples.
     */
    virtual void betweenStages(int stageIndex, float* buffer, int numSamples) = 0;
};

/**
 * Pipeline: ordered chain of ProcessorStages with optional controller hooks.
 *
 * - Does not own stages or controllers -- they are owned by the host/setup.
 * - Stages are set at initialization time and do not change during processing.
 * - The stage list is a fixed-capacity array (no heap allocation).
 * - Controllers are notified at preProcess and betweenStages points.
 *
 * Signal flow per block:
 *   1. controller->preProcess(buffer) for each controller
 *   2. for each stage i:
 *        stage[i]->process(buffer)
 *        controller->betweenStages(i, buffer) for each controller
 *
 * Thread safety:
 *   - prepare(), addStage(), addController() are non-RT, called before audio.
 *   - process() is called from the audio thread only.
 *   - reset() is RT-safe.
 */
class Pipeline {
public:
    static constexpr int kMaxStages      = 16;
    static constexpr int kMaxControllers = 4;

    Pipeline() = default;

    // Non-copyable, non-moveable (owns no resources but stages hold state)
    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /**
     * Add a stage to the end of the chain. Not real-time safe.
     * Must be called before prepare().
     */
    void addStage(ProcessorStage* stage);

    /**
     * Add a pipeline controller. Not real-time safe.
     * Must be called before prepare().
     */
    void addController(PipelineController* controller);

    /**
     * Prepare all stages. Not real-time safe.
     */
    void prepare(float sampleRate, int maxBlockSize);

    /**
     * Process one block of audio. Real-time safe.
     */
    void process(float* buffer, int numSamples);

    /**
     * Reset all stages. Real-time safe.
     */
    void reset();

    int numStages()      const { return numStages_; }
    int numControllers() const { return numControllers_; }

private:
    std::array<ProcessorStage*,      kMaxStages>      stages_      = {};
    std::array<PipelineController*,  kMaxControllers> controllers_ = {};
    int   numStages_      = 0;
    int   numControllers_ = 0;
    float sampleRate_     = 0.f;
    int   maxBlockSize_   = 0;
};

} // namespace hexcaster
