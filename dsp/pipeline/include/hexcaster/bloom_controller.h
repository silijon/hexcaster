#pragma once

#include "hexcaster/pipeline.h"
#include "hexcaster/param_registry.h"

namespace hexcaster {

class GainStage;
class EnvelopeFollower;

/**
 * BloomController: dynamic gain coordinator.
 *
 * Implements the core "Bloom" design: a single envelope follower drives
 * both pre-amp and post-amp gain in opposite directions, maintaining
 * perceived volume while modulating the amp model's input level.
 *
 *   PreGain_dB  = BasePre  - A * envelope
 *   PostGain_dB = BasePost + B * envelope
 *
 * Both values are clamped to safe limits.
 *
 * Architecture:
 *   - Registered as a PipelineController.
 *   - preProcess(): runs envelope follower on the input signal.
 *   - betweenStages(): applies pre-gain before the amp stage (stageIndex == preAmpStageIndex - 1)
 *                      applies post-gain after the amp stage (stageIndex == ampStageIndex).
 *   - Reads parameters from ParamRegistry each block (atomic reads).
 *   - Writes to GainStage via setGainLinear() (pre-amp and post-amp GainStage references).
 *
 * The BloomController does NOT own the GainStage objects -- those live in
 * the pipeline stage list. This is intentional: you can swap the gain
 * application strategy (e.g., sample-by-sample vs. block-level) without
 * changing the controller interface.
 *
 * Implementation: to be added in Phase 1.
 */
class BloomController : public PipelineController {
public:
    /**
     * @param preGainStage        GainStage in the pipeline that precedes the amp model.
     * @param postGainStage       GainStage in the pipeline that follows the amp model.
     * @param preStageIndex       Pipeline stage index of preGainStage (controller injects after this - 1).
     * @param postStageIndex      Pipeline stage index of postGainStage (controller injects after this).
     * @param registry            Parameter source (read-only from audio thread).
     */
    BloomController(GainStage&      preGainStage,
                    GainStage&      postGainStage,
                    int             preStageIndex,
                    int             postStageIndex,
                    ParamRegistry&  registry);

    void prepare(float sampleRate, int maxBlockSize);

    void preProcess(const float* buffer, int numSamples) override;
    void betweenStages(int stageIndex, float* buffer, int numSamples) override;

    void reset();

private:
    GainStage&     preGainStage_;
    GainStage&     postGainStage_;
    int            preStageIndex_;
    int            postStageIndex_;
    ParamRegistry& registry_;
    float          envelope_ = 0.f;
};

} // namespace hexcaster
