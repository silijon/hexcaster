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
    void setActivityThreshold(float t);   // Adaptive release gate [0, 1]

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
     * Read the current harmonic activity level.
     * EMA of |delta(smoothedDet)|. High = complex harmonic content (chords).
     * Low = simple content or silence (single notes, quiet).
     * Computed in all modes; used by Adaptive mode for release decisions.
     * Safe to call from any thread (relaxed atomic load).
     */
    float getHarmonicActivity() const;

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
    std::atomic<float>   sensitivity_         { 10.f  }; // dB
    std::atomic<float>   activityThreshold_   { 0.01f }; // Adaptive mode release gate [0,1]

    // --- Observation atomics (written by audio thread, read by TUI thread) ---
    // Updated once per block at the end of preProcess(). Relaxed ordering.
    std::atomic<float> observedDetectorRawEnvelope_ { 0.f };  // raw detector (pre-smoothing)
    std::atomic<float> observedDetectorEnvelope_ { 0.f };  // fast detector (tracks audio)
    std::atomic<float> observedDetectorPeak_     { 0.f };  // last peak (tracks audio)
    std::atomic<float> observedGainEnvelope_     { 0.f };  // gain envelope (drives bloom gains)
    std::atomic<float> observedHarmonicActivity_ { 0.f };  // harmonic activity metric

    // --- Audio thread state ---
    float sampleRate_ = 48000.f;

    // -----------------------------------------------------------------------
    // Stage 1: fast peak detector (fixed time constants, not user-controlled)
    // -----------------------------------------------------------------------
    static constexpr float kDetectorHpfHz        = 200.f;   // 1st-order high-pass, 200 Hz fixed
    static constexpr float kDetectorAttackMs     =   0.1f;  // near-instantaneous peak capture
    static constexpr float kDetectorReleaseMs    =  30.f;   // total release duration (ms)
    static constexpr float kDetectorSmoothMs     =  25.f;   // one-pole LPF on detector output

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

    // -----------------------------------------------------------------------
    // Harmonic activity metric (computed in all modes, used by Adaptive)
    //
    // Running EMA of |delta(smoothedDet)|. Measures how much the detector
    // signal is fluctuating. High = complex harmonic content (chord beating).
    // Low = clean single-note decay or silence.
    //
    // In Adaptive mode, this determines release behaviour:
    //   harmAct > threshold → track audio (chord-like, gentle release)
    //   harmAct <= threshold → freefall at user release rate (single note)
    // -----------------------------------------------------------------------
    static constexpr float kActivityFollowerMs = 50.f;    // EMA time constant
    // Squared delta scaling: brings (absDelta^2) into a [0,1] display range.
    // absDelta for chords is ~1e-5, squared ~1e-10, * 1e9 => ~0.1 on meter.
    // absDelta for single notes is ~1e-6, squared ~1e-12, * 1e9 => ~0.001.
    static constexpr float kActivityScale      = 1e9f;    // scale squared delta
    float harmonicActivity_      = 0.f;
    float prevSmoothedDet_       = 0.f;   // previous sample's smoothedDet (for delta)
    float activityCoeff_         = 0.f;   // computed once in prepare()


    // -----------------------------------------------------------------------
    // Stage 2: gain envelope (parabolic decay, user BloomAttackMs / BloomReleaseMs control this)
    // Release uses an explicit parabolic decay: detEnv = peak * (1 - t/T)^n
    // where t is samples elapsed since release start and T is the total
    // release duration. This is guaranteed concave: zero slope at the peak
    // (slow plateau), steepening toward zero. Unlike any exponential variant,
    // this directly evaluates a concave function rather than trying to reshape
    // a convex one.
    // -----------------------------------------------------------------------
    // TODO: maybe make this user-controlled 1-10?
    static constexpr float kGainCompReleasePower =   3.f;  // curve exponent (>1 for concave, higher = longer plateau)

    int   gainEnvAttackCoeff_    = 0.f;     // computed once in prepare()
    float gainEnvReleaseDur_     = 0.1f;    // total release duration in samples (set in prepare)
    float gainEnv_               = 0.f;     // realtime gain envelope value
    int   gainEnvReleaseSample_  = 0;       // samples elapsed since release started
    bool  gainEnvReleasing_      = false;   // true while in release phase

    float cachedAttackMs_    = -1.f;
    float cachedReleaseMs_   = -1.f;

    // gain envelope state machine
    enum class GainEnvState : uint8_t { Attack, Release };
    GainEnvState gainEnvState_ = GainEnvState::Release;

    void updateCoefficients();
    void computeHpfCoefficients();

    static float msToCoeff(float ms, float sampleRate);
};

} // namespace hexcaster
