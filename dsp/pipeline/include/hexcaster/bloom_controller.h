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
 *   reductionDb = BloomDepth_dB * gainEnvelope
 *   preGainDb   = BloomBasePre_dB  - reductionDb
 *   postGainDb  = BloomBasePost_dB + BloomCompensation * reductionDb
 *
 * When gainEnvelope = 0 (silence): pre-gain = BasePre, post-gain = BasePost.
 * When gainEnvelope = 1 (full):    pre-gain = BasePre - Depth,
 *                                  post-gain = BasePost + Compensation * Depth.
 *
 * Architecture:
 *   - Registered as a PipelineController via pipeline.addController().
 *   - preProcess(): runs detector HPF + two-stage envelope on the input
 *     signal (read-only), then sets the pre-gain and post-gain targets
 *     on the GainStage references. The GainStages then apply smoothed
 *     gain per-sample when their process() is called in the stage chain.
 *   - betweenStages(): no-op. All work is done in preProcess().
 *   - Does NOT own the GainStage objects -- they live in the pipeline
 *     stage list. The host creates them and passes references.
 *
 * Two-stage envelope:
 *   Stage 1 -- Detector (fast, fixed time constants):
 *     Instantaneous peak tracking with fixed short attack (~0.1 ms) and
 *     release (~10 ms). Answers: "is there signal, and how strong is it?"
 *     Not controlled by user parameters.
 *
 *   Stage 2 -- Gain envelope (user-controlled attack/release):
 *     EMA that tracks the detector output. BloomAttackMs and BloomReleaseMs
 *     control how fast the gain ramps in response to the detector signal.
 *     This is the value that drives the gain formulas and is shown in the TUI.
 *
 *   Separation rationale: the detector fires immediately when a note is
 *   struck; the gain envelope then shapes how quickly the pre/post gains
 *   respond. Attack/release therefore control the musical gain behaviour,
 *   not how quickly the detector tracks the audio waveform.
 *
 *   Detector HPF (1st-order high-pass at 100 Hz, fixed) applied to the
 *   detection signal only, not the audio path. Prevents low-frequency
 *   thumps from dominating the detector.
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
    void setSensitivity(float db);  // detection signal gain [0, 40] dB

    /**
     * Read the current envelope follower value [0.0, 1.0].
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block at the end of preProcess().
     * Intended for TUI metering only -- do not use in the audio path.
     */
    float getEnvelope() const;

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
    std::atomic<float> sensitivity_  { 20.f  }; // dB

    // --- Observation atomic (written by audio thread, read by TUI thread) ---
    // Updated once per block at the end of preProcess(). Relaxed ordering.
    std::atomic<float> observedEnvelope_{ 0.f };

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // Stage 1: fast peak detector (fixed time constants, not user-controlled)
    static constexpr float kDetectorAttackMs  = 0.1f;   // near-instantaneous peak capture
    static constexpr float kDetectorReleaseMs = 10.f;   // fast release to track note endings
    float detectorEnv_           = 0.f;
    float detectorAttackCoeff_   = 0.f;   // computed once in prepare()
    float detectorReleaseCoeff_  = 0.f;   // computed once in prepare()

    // Stage 2: gain envelope (user BloomAttackMs / BloomReleaseMs control this)
    float gainEnv_           = 0.f;
    float gainAttackCoeff_   = 0.f;   // recomputed when params change
    float gainReleaseCoeff_  = 0.f;   // recomputed when params change
    float cachedAttackMs_    = -1.f;
    float cachedReleaseMs_   = -1.f;

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
