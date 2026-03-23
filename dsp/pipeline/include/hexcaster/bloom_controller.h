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
/**
 * BloomMode selects how the gain envelope responds to the detector.
 *
 *   Shaped:   State-machine driven. Attack ramps toward detector on new note
 *             onset; release decays toward zero independently of the audio.
 *             Clean, predictable attack/release contours.
 *
 *   Tracking: Gain envelope smoothly follows the detector using the user's
 *             attack/release as a two-pole smoother. Gain tracks the audio
 *             dynamics rather than imposing an independent decay shape.
 */
enum class BloomMode : uint8_t { Shaped = 0, Tracking = 1 };

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

    /** Set the bloom gain envelope mode. Thread-safe. */
    void setMode(BloomMode m);

    /** Read the current bloom mode. Thread-safe. */
    BloomMode getMode() const;

    /**
     * Read the current gain envelope value [0.0, 1.0].
     * This is what drives the bloom pre/post gains. Its shape is governed
     * by BloomAttackMs and BloomReleaseMs.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block at the end of preProcess().
     * Intended for TUI metering only -- do not use in the audio path.
     */
    float getEnvelope() const;

    /**
     * Read the current fast detector envelope value [0.0, 1.0].
     * This tracks the raw audio amplitude with fixed short time constants.
     * Useful for TUI visualization to compare against the gain envelope.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block.
     */
    float getDetectorEnvelope() const;

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
    std::atomic<uint8_t> mode_       { static_cast<uint8_t>(BloomMode::Shaped) };

    // --- Observation atomics (written by audio thread, read by TUI thread) ---
    // Updated once per block at the end of preProcess(). Relaxed ordering.
    std::atomic<float> observedEnvelope_    { 0.f };  // gain envelope (drives bloom gains)
    std::atomic<float> observedDetectorEnv_ { 0.f };  // fast detector (tracks audio)

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // Stage 1: fast peak detector (fixed time constants, not user-controlled)
    static constexpr float kDetectorAttackMs  =  0.1f;  // near-instantaneous peak capture
    static constexpr float kDetectorReleaseMs = 30.f;   // fast-ish release; bumped from 10ms
                                                         // to reduce per-cycle ripple on sustain
    static constexpr float kDetectorSmoothMs  = 15.f;   // one-pole LPF on detector output
                                                         // removes residual ripple before
                                                         // feeding the gain envelope
    float detectorEnv_           = 0.f;
    float smoothedDetEnv_        = 0.f;   // LPF-smoothed detector output
    float detectorAttackCoeff_   = 0.f;   // computed once in prepare()
    float detectorReleaseCoeff_  = 0.f;   // computed once in prepare()
    float detectorSmoothCoeff_   = 0.f;   // computed once in prepare()

    // Stage 2: gain envelope (user BloomAttackMs / BloomReleaseMs control this)
    float gainEnv_           = 0.f;
    float gainAttackCoeff_   = 0.f;   // recomputed when params change
    float gainReleaseCoeff_  = 0.f;   // recomputed when params change
    float cachedAttackMs_    = -1.f;
    float cachedReleaseMs_   = -1.f;

    // -----------------------------------------------------------------------
    // Shaped mode: energy-ratio transient detector
    //
    // Compares short-term energy (fast follower, ~3ms) to long-term energy
    // (slow follower, ~150ms). A note onset produces a large ratio (short
    // jumps while long is still low); chord beating during sustain produces
    // a ratio near 1.0 (both followers track similar levels).
    //
    // When the ratio exceeds kOnsetRatioThreshold, shapedDet is set to the
    // current signal level. Otherwise shapedDet decays to zero at
    // kShapedDetReleaseMs. The gain envelope state machine is driven by
    // shapedDet, so it can release cleanly to zero without re-triggering
    // on sustained audio or chord beating.
    // -----------------------------------------------------------------------
    static constexpr float kFastEnergyMs         =   3.f;  // short-term follower
    static constexpr float kSlowEnergyMs         = 150.f;  // long-term follower
    static constexpr float kOnsetRatioThreshold  =   2.5f; // fast/slow ratio for onset
    static constexpr float kShapedDetReleaseMs   =  20.f;  // shapedDet decay to zero

    // Per-sample delta threshold for detecting new notes over existing signal.
    // A note onset rises at ~0.002/sample; chord beating at ~0.00001/sample.
    // 0.001 gives a 100x margin above beating while catching moderate onsets.
    static constexpr float kFastDeltaThreshold   = 0.001f;

    float fastEnergy_            = 0.f;
    float slowEnergy_            = 0.f;
    float shapedDet_             = 0.f;
    float prevSmoothedDet_       = 0.f;   // previous sample's smoothedDet (for delta)
    float fastEnergyCoeff_       = 0.f;   // computed once in prepare()
    float slowEnergyCoeff_       = 0.f;   // computed once in prepare()
    float shapedDetReleaseCoeff_ = 0.f;   // computed once in prepare()

    // Shaped mode state machine
    enum class GainEnvState : uint8_t { Attack, Release };
    GainEnvState gainEnvState_ = GainEnvState::Release;

    // Onset detection threshold for Shaped mode: shapedDet must exceed the
    // gain envelope by this amount to trigger attack. Filters out noise.
    static constexpr float kOnsetThreshold = 0.05f;

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
