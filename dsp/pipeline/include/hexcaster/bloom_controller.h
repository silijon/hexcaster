#pragma once

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"

#include <atomic>

namespace hexcaster {

/**
 * BloomController: dynamic gain coordinator.
 *
 * Implements the "Bloom" system: a single envelope follower drives
 * pre-amp and post-amp gain in opposite directions, maintaining
 * perceived volume while dynamically modulating the NAM model's input level.
 *
 * Gain formulas (evaluated once per block):
 *
 *   reductionDb = BloomDepth * envelope
 *   preGainDb   = BloomBasePre_dB  - reductionDb
 *   postGainDb  = BloomBasePost_dB + BloomCompensation * reductionDb
 *
 * When envelope = 0 (silence): pre-gain = BasePre, post-gain = BasePost.
 * When envelope = 1 (full):    pre-gain = BasePre - Depth,
 *                               post-gain = BasePost + Compensation * Depth.
 *
 * Architecture:
 *   - Registered as a PipelineController via pipeline.addController().
 *   - preProcess(): runs detector HPF + envelope follower on the input
 *     signal (read-only), then sets the pre-gain and post-gain targets
 *     on the GainStage references. The GainStages then apply smoothed
 *     gain per-sample when their process() is called in the stage chain.
 *   - betweenStages(): no-op. All work is done in preProcess().
 *   - Does NOT own the GainStage objects -- they live in the pipeline
 *     stage list. The host creates them and passes references.
 *
 * Envelope follower:
 *   - Per-sample peak tracking with configurable attack/release.
 *   - Detector HPF (1st-order high-pass at 100 Hz, fixed) applied to the
 *     detection signal only, not the audio path. Prevents low-frequency
 *     thumps from dominating the envelope.
 *   - Output normalised to [0.0, 1.0].
 *
 * Real-time safety:
 *   - preProcess() is RT-safe: no allocation, no I/O, bounded time.
 *   - Atomic params read once per block, not per-sample.
 */
class BloomController : public PipelineController {
public:
    /**
     * @param preGain   GainStage that precedes the NAM model in the pipeline.
     * @param postGain  GainStage that follows the NAM model in the pipeline.
     */
    BloomController(GainStage& preGain, GainStage& postGain);

    /**
     * Prepare internal state. Not real-time safe.
     * Must be called before the audio thread starts.
     */
    void prepare(float sampleRate, int maxBlockSize);

    /**
     * Reset envelope and HPF state. Real-time safe.
     */
    void reset();

    // PipelineController interface
    void preProcess(const float* buffer, int numSamples) override;
    void betweenStages(int stageIndex, float* buffer, int numSamples) override;

    // Control thread setters (atomic)
    void setBasePreDb(float db);
    void setBasePostDb(float db);
    void setDepth(float db);
    void setCompensation(float ratio);
    void setAttackMs(float ms);
    void setReleaseMs(float ms);

private:
    GainStage& preGain_;
    GainStage& postGain_;

    // --- Atomic parameters (control thread writes, audio thread reads) ---
    std::atomic<float> basePreDb_    { 0.f   };
    std::atomic<float> basePostDb_   { 0.f   };
    std::atomic<float> depth_        { 6.f   };
    std::atomic<float> compensation_ { 0.5f  };
    std::atomic<float> attackMs_     { 5.f   };
    std::atomic<float> releaseMs_    { 100.f };

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;
    float envelope_   = 0.f;     // current peak envelope [0, 1]

    // Envelope follower EMA coefficients (recomputed when params change)
    float attackCoeff_  = 0.f;
    float releaseCoeff_ = 0.f;
    float cachedAttackMs_  = -1.f;
    float cachedReleaseMs_ = -1.f;

    // Detector HPF state (1st-order high-pass, 100 Hz fixed)
    static constexpr float kDetectorHpfHz = 100.f;
    float hpfX1_ = 0.f;   // previous input sample
    float hpfY1_ = 0.f;   // previous output sample
    float hpfA1_ = 0.f;   // feedback coefficient
    float hpfB0_ = 0.f;   // feedforward coefficient
    float hpfB1_ = 0.f;   // feedforward coefficient (x[n-1])

    void updateCoefficients();
    void computeHpfCoefficients();

    static float msToCoeff(float ms, float sampleRate);
};

} // namespace hexcaster
