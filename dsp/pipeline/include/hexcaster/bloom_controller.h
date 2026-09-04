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
    void setSensitivity(float db);         // detection signal gain [0, 40] dB
    void setObservationEnabled(bool enabled);

    /**
     * Read the current gain envelope value [0.0, 1.0].
     * This is what drives the bloom pre/post gains. Its shape is governed
     * by BloomAttackMs and BloomReleaseMs.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block at the end of preProcess().
     * Intended for TUI metering only -- do not use in the audio path.
     */
    float getGainEnvelope() const;

    /**
     * Read the current fast detector envelope value [0.0, 1.0].
     * This tracks the raw audio amplitude with fixed short time constants.
     * Useful for TUI visualization to compare against the gain envelope.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block.
     */
    float getDetectorEnvelope() const;

    /**
     * Read the current raw (pre-smoothing) detector envelope value [0.0, 1.0].
     * This is the output of the fast peak detector before the LPF smoothing
     * stage. Useful for comparing against the smoothed detector envelope.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block.
     */
    float getDetectorRawEnvelope() const;

    /**
     * Read the current fast detector peak value [0.0, 1.0].
     * This tracks the raw audio amplitude with fixed short time constants.
     * Useful for TUI visualization to see which value is driving the gain.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block.
     */
    float getDetectorPeak() const;

    /**
     * Read the current fast detector slope value [0.0, 1.0].
     * This tracks the onset and decay rate of the transient.
     * Useful for TUI visualization to see which value is driving the gain.
     * Safe to call from any thread (relaxed atomic load).
     * Updated once per audio block.
     */
    float getDetectorSlope() const;

    /**
     * Read the current chord score [0, 1].
     * Combined peak + max-slope feature with dynamic weighting based on
     * inter-transient time. Drives the gain envelope decay rate. Updated
     * once per audio block. Safe to call from any thread.
     */
    float getChordScore() const;

private:
    GainStage& preGain_;
    GainStage& postGain_;

    // --- Atomic parameters (control thread writes, audio thread reads) ---
    std::atomic<float>   basePreDb_           { 0.f  };
    std::atomic<float>   basePostDb_          { 0.f  };
    std::atomic<float>   depth_               { 24.f };
    std::atomic<float>   compensation_        { 0.5f };
    std::atomic<float>   attackMs_            { 5.f  };
    std::atomic<float>   releaseMs_           { 5.f  };
    std::atomic<float>   sensitivity_         { 5.f  }; // dB
    std::atomic<bool>    observationEnabled_  { true };

    // --- Observation atomics (written by audio thread, read by TUI thread) ---
    // Updated once per block at the end of preProcess(). Relaxed ordering.
    std::atomic<float> observedDetectorRawEnvelope_ { 0.f };  // raw detector (pre-smoothing)
    std::atomic<float> observedDetectorEnvelope_    { 0.f };  // fast detector (tracks audio)
    std::atomic<float> observedDetectorSlope_       { 0.f };  // last delta (tracks audio)
    std::atomic<float> observedDetectorPeak_        { 0.f };  // last peak (tracks audio)
    std::atomic<float> observedGainEnvelope_        { 0.f };  // gain envelope (drives bloom gains)
    std::atomic<float> observedChordScore_          { 0.f };  // combined peak+slope chord score

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // -----------------------------------------------------------------------
    // Stage 1: fast peak detector (fixed time constants, not user-controlled)
    // -----------------------------------------------------------------------
    static constexpr float kDetectorHpfHz        = 200.f;   // 1st-order high-pass, 200 Hz fixed
    static constexpr float kDetectorAttackMs     =   0.1f;  // near-instantaneous peak capture
    static constexpr float kDetectorReleaseMs    =  30.f;   // total release duration (ms)
    static constexpr float kDetectorSmoothMs     =  70.f;   // one-pole LPF on detector output
    static constexpr int   kDetectorHoldoffSamples =  512;  // how many samples to holdoff before calling ADSR phase change 

    float hpfX1_ = 0.f;                    // previous input sample
    float hpfY1_ = 0.f;                    // previous output sample
    float hpfA1_ = 0.f;                    // feedback coefficient
    float hpfB0_ = 0.f;                    // feedforward coefficient
    float hpfB1_ = 0.f;                    // feedforward coefficient (x[n-1])
    float detectorAttackCoeff_   = 0.f;    // computed once in prepare()
    float detectorReleaseCoeff_  = 0.f;    // computed once in prepare()
    float detectorRawEnv_        = 0.f;    // current detector output (linear)
    float detectorSmoothCoeff_   = 0.f;    // computed once in prepare()
    float detectorSmoothEnv_     = 0.f;    // LPF-smoothed detector output
    float detectorPeak_          = 0.f;    // peak value captured at release start
    float detectorSlope_         = 0.f;    // change in sample amplitude (for delta)
    float detectorMaxSlope_   = 0.f;    // max slope achieved by a note on the rising edge (for delta)
    bool  detectorUnderAttack_   = false;   // state tracking for the attack phase of transient
    int   detectorHoldoffCounter_ = kDetectorHoldoffSamples;

    // -----------------------------------------------------------------------
    // Combined peak + slope chord score (drives gain decay rate)
    //
    // Dynamic weighting between detPeak and detMaxSlope based on the
    // interval since the previous transient. Long intervals → trust slope
    // (dynamics-invariant). Short intervals → trust peak (slope is
    // suppressed when notes pile up). Result is rescaled by kPeakRef so
    // chord-typical values land near the existing 0.5 pivot of the gain
    // formula.
    // -----------------------------------------------------------------------
    static constexpr float kPeakRef            = 0.6f;     // chord-typical peak (matches existing gain formula pivot)
    static constexpr float kSlopeRef           = 0.0004f;   // chord-typical detMaxSlope (tune from debug captures)
    static constexpr float kFastNoteMs         = 500.f;    // below this, weight toward peak
    static constexpr float kChordFloor         = 0.05f;    // min chord score (prevents pow() blow-up)
    static constexpr float kChordScoreSmoothMs = 10.f;     // EMA on chord score target

    int   samplesSinceLastTransient_ = 0;     // counter, incremented every sample, reset at onset
    int   lastTransientInterval_     = 0;     // captured interval at each onset (for dynamic weighting)
    float chordScoreTarget_          = 0.f;   // chord score computed at end-of-attack, held until next
    float chordScoreSmoothed_        = 0.f;   // per-sample EMA of target, used by gain formula
    float chordScoreSmoothCoeff_     = 0.f;   // computed once in prepare()

    // Stage 2: gain envelope. The default build follows the detector with
    // user-controlled attack/release coefficients. Experimental builds use
    // releaseMs as the shaping exponent in the chord-score power curve.
    float gainEnv_             = 0.f;
    float gainEnvAttackCoeff_  = 0.f;
    float gainEnvReleaseCoeff_ = 0.f;
    // Experimental path only: the numeric BloomReleaseMs value is reused as
    // a dimensionless exponent in the chord-score power curve. It is not a
    // duration or EMA coefficient in that path; larger values reshape the
    // detector response more aggressively. The distinct name prevents it
    // from being confused with gainEnvReleaseCoeff_, used by the default path.
    float gainEnvReleaseShape_ = 0.1f;

    float cachedAttackMs_    = -1.f;
    float cachedReleaseMs_   = -1.f;
    float cachedSensitivityDb_ = -1.f;
    float sensitivityLin_      = 1.f;

    void updateCoefficients();
    void computeHpfCoefficients();

    static float msToCoeff(float ms, float sampleRate);
};

} // namespace hexcaster
