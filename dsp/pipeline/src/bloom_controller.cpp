#include "hexcaster/bloom_controller.h"

#include <algorithm>
#include <cmath>

namespace hexcaster {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

BloomController::BloomController(GainStage& preGain, GainStage& postGain)
    : preGain_(preGain)
    , postGain_(postGain)
{
}

// ---------------------------------------------------------------------------
// prepare / reset
// ---------------------------------------------------------------------------

void BloomController::prepare(float sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    computeHpfCoefficients();

    // Compute fixed detector coefficients (not user-controlled)
    detectorAttackCoeff_     = msToCoeff(kDetectorAttackMs,     sampleRate_);
    detectorReleaseCoeff_    = msToCoeff(kDetectorReleaseMs,    sampleRate_);
    detectorSmoothCoeff_     = msToCoeff(kDetectorSmoothMs,     sampleRate_);
    shapedDetReleaseCoeff_   = msToCoeff(kShapedDetReleaseMs,   sampleRate_);

    cachedAttackMs_  = -1.f;  // force gain envelope coefficient recompute on first block
    cachedReleaseMs_ = -1.f;
    reset();
}

void BloomController::reset()
{
    detectorEnv_       = 0.f;
    smoothedDetEnv_    = 0.f;
    shapedDet_         = 0.f;
    prevSmoothedDet_   = 0.f;
    gainEnv_           = 0.f;
    gainEnvState_      = GainEnvState::Release;
    hpfX1_             = 0.f;
    hpfY1_             = 0.f;
}

// ---------------------------------------------------------------------------
// PipelineController interface
// ---------------------------------------------------------------------------

void BloomController::preProcess(const float* buffer, int numSamples)
{
    // Read all atomic params once per block
    const float basePreDb    = basePreDb_.load(std::memory_order_relaxed);
    const float basePostDb   = basePostDb_.load(std::memory_order_relaxed);
    const float depth        = depth_.load(std::memory_order_relaxed);
    const float compensation = compensation_.load(std::memory_order_relaxed);
    const float attackMs     = attackMs_.load(std::memory_order_relaxed);
    const float releaseMs    = releaseMs_.load(std::memory_order_relaxed);
    const float sensitivityLin = std::pow(10.f,
        sensitivity_.load(std::memory_order_relaxed) / 20.f);

    // Recompute EMA coefficients if attack/release changed
    if (attackMs != cachedAttackMs_ || releaseMs != cachedReleaseMs_) {
        cachedAttackMs_  = attackMs;
        cachedReleaseMs_ = releaseMs;
        updateCoefficients();
    }

    // Run detector HPF + two-stage envelope per-sample across the block.
    // The HPF is applied to the detection signal only -- the audio buffer
    // is const and is not modified.
    const auto mode = static_cast<BloomMode>(mode_.load(std::memory_order_relaxed));

    float detEnv      = detectorEnv_;
    float smoothedDet = smoothedDetEnv_;
    float sDet        = shapedDet_;
    float prevSD      = prevSmoothedDet_;
    float gainEnv     = gainEnv_;
    auto  gainEnvSt   = gainEnvState_;

    for (int i = 0; i < numSamples; ++i) {
        // Stage 1a: detector HPF (sidechain only, not audio path)
        const float x      = buffer[i];
        const float hpfOut = hpfB0_ * x + hpfB1_ * hpfX1_ - hpfA1_ * hpfY1_;
        hpfX1_ = x;
        hpfY1_ = hpfOut;

        // Stage 1b: fast peak detector.
        // Sensitivity scales the detection signal to normalise raw ADC amplitude.
        // Near-instantaneous attack (0.1ms) captures note onsets immediately.
        // Release is 30ms -- fast enough to track amplitude changes but slow
        // enough to reduce per-cycle harmonic ripple on sustained notes.
        const float absSample = (hpfOut < 0.f ? -hpfOut : hpfOut) * sensitivityLin;
        if (absSample > detEnv)
            detEnv = detectorAttackCoeff_  * detEnv + (1.f - detectorAttackCoeff_)  * absSample;
        else
            detEnv = detectorReleaseCoeff_ * detEnv + (1.f - detectorReleaseCoeff_) * absSample;

        // Stage 1c: one-pole LPF on detector output (15ms).
        // Removes residual harmonic-beating ripple from the detector signal.
        smoothedDet = detectorSmoothCoeff_ * smoothedDet
                    + (1.f - detectorSmoothCoeff_) * detEnv;

        // Stage 2: gain envelope.
        // Mode determines how the gain envelope responds to the detector.
        if (mode == BloomMode::Shaped) {
            // Stage 1d (Shaped only): delta-based transient detector.
            // Accumulates the positive rate of change of smoothedDet.
            // Only grows while the signal is actively rising (note onset).
            // Sustain and natural decay produce zero/negative delta, so
            // shapedDet decays freely to zero regardless of audio level.
            const float delta = smoothedDet - prevSD;
            // Allow transient accumulation when:
            //   - gainEnv has substantially released (near silence) -- normal onset, OR
            //   - the delta is very large (unmistakably a new note strike) -- force onset.
            // This prevents chord beating (small deltas during elevated gainEnv)
            // from re-triggering shapedDet, while still catching fast repeated notes.
            const bool canTrigger  = (gainEnv < kRetriggerThreshold);
            const bool forceOnset  = (delta   > kForceOnsetDelta);
            if ((canTrigger || forceOnset) && delta > 0.f)
                sDet = std::min(sDet + delta, 1.f);
            else
                sDet = shapedDetReleaseCoeff_ * sDet;

            // Shaped gain envelope: state-machine driven by shapedDet.
            // Because shapedDet decays to zero after the transient, the
            // gain envelope can release all the way to zero at the user's
            // rate without being re-triggered by sustained audio.
            if (gainEnvSt == GainEnvState::Release) {
                gainEnv = gainReleaseCoeff_ * gainEnv;
                if (sDet > gainEnv + kOnsetThreshold)
                    gainEnvSt = GainEnvState::Attack;
            }
            if (gainEnvSt == GainEnvState::Attack) {
                gainEnv = gainAttackCoeff_ * gainEnv + (1.f - gainAttackCoeff_) * sDet;
                if (sDet < gainEnv)
                    gainEnvSt = GainEnvState::Release;
            }
        } else {
            // Tracking: smoothly follow detector using user attack/release.
            // Release targets smoothedDet (tracks audio), not zero.
            if (smoothedDet > gainEnv)
                gainEnv = gainAttackCoeff_  * gainEnv + (1.f - gainAttackCoeff_)  * smoothedDet;
            else
                gainEnv = gainReleaseCoeff_ * gainEnv + (1.f - gainReleaseCoeff_) * smoothedDet;
        }

        // Always update prevSD so it's valid if user switches modes mid-session
        prevSD = smoothedDet;
    }

    detectorEnv_       = detEnv;
    smoothedDetEnv_    = smoothedDet;
    shapedDet_         = sDet;
    prevSmoothedDet_   = prevSD;
    gainEnv_           = gainEnv;
    gainEnvState_      = gainEnvSt;

    // Publish envelopes for TUI observation (relaxed -- no ordering required).
    // In Shaped mode, show the transient detector (shapedDet) so you can see
    // it pulse on onset and decay to zero. In Tracking mode, show smoothedDet
    // (continuous audio tracker). The gain envelope is always shown.
    const float detForTui = (mode == BloomMode::Shaped)
                          ? std::clamp(sDet, 0.f, 1.f)
                          : std::clamp(smoothedDet, 0.f, 1.f);
    observedDetectorEnv_.store(detForTui, std::memory_order_relaxed);
    observedEnvelope_.store(std::clamp(gainEnv, 0.f, 1.f), std::memory_order_relaxed);

    // Clamp for the gain formulas (safety net -- gainEnv should already be [0,1]).
    const float clampedEnv = std::clamp(gainEnv, 0.f, 1.f);

    // Compute gain targets from the envelope
    const float reductionDb = depth * clampedEnv;
    const float preGainDb   = basePreDb  - reductionDb;
    const float postGainDb  = basePostDb + compensation * reductionDb;

    // Set the gain stages. Their internal smoothers will interpolate
    // per-sample from the previous target to this new target.
    preGain_.setGainDb(preGainDb);
    postGain_.setGainDb(postGainDb);
}

void BloomController::betweenStages(int /*stageIndex*/, float* /*buffer*/,
                                     int /*numSamples*/)
{
    // No-op. All work is done in preProcess().
}

// ---------------------------------------------------------------------------
// Parameter setters
// ---------------------------------------------------------------------------

float BloomController::getEnvelope() const
{
    return observedEnvelope_.load(std::memory_order_relaxed);
}

float BloomController::getDetectorEnvelope() const
{
    return observedDetectorEnv_.load(std::memory_order_relaxed);
}

void BloomController::setBasePreDb(float db)
{
    basePreDb_.store(std::clamp(db, -24.f, 24.f), std::memory_order_relaxed);
}

void BloomController::setBasePostDb(float db)
{
    basePostDb_.store(std::clamp(db, -24.f, 24.f), std::memory_order_relaxed);
}

void BloomController::setDepth(float db)
{
    depth_.store(std::clamp(db, 0.f, 24.f), std::memory_order_relaxed);
}

void BloomController::setCompensation(float ratio)
{
    compensation_.store(std::clamp(ratio, 0.f, 2.f), std::memory_order_relaxed);
}

void BloomController::setAttackMs(float ms)
{
    attackMs_.store(std::clamp(ms, 0.1f, 500.f), std::memory_order_relaxed);
}

void BloomController::setReleaseMs(float ms)
{
    releaseMs_.store(std::clamp(ms, 1.f, 5000.f), std::memory_order_relaxed);
}

void BloomController::setSensitivity(float db)
{
    sensitivity_.store(std::clamp(db, 0.f, 40.f), std::memory_order_relaxed);
}

void BloomController::setMode(BloomMode m)
{
    mode_.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
}

BloomMode BloomController::getMode() const
{
    return static_cast<BloomMode>(mode_.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

float BloomController::msToCoeff(float ms, float sampleRate)
{
    if (ms <= 0.f || sampleRate <= 0.f) return 0.f;
    return std::exp(-1.f / (ms * 0.001f * sampleRate));
}

void BloomController::updateCoefficients()
{
    // Update gain envelope coefficients from user attack/release params.
    // Detector coefficients are fixed and set once in prepare().
    gainAttackCoeff_  = msToCoeff(cachedAttackMs_,  sampleRate_);
    gainReleaseCoeff_ = msToCoeff(cachedReleaseMs_, sampleRate_);
}

// Detector HPF: 1st-order high-pass filter.
// Transfer function: H(z) = (1 + z^-1) * g / (1 + a1*z^-1)
// Using bilinear transform of s-domain HPF at kDetectorHpfHz.
void BloomController::computeHpfCoefficients()
{
    const float w0 = 2.f * static_cast<float>(M_PI) * kDetectorHpfHz / sampleRate_;
    const float alpha = std::cos(w0) / (1.f + std::sin(w0));

    // 1st-order HPF coefficients (normalised):
    //   y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
    hpfB0_ =  (1.f + alpha) / 2.f;
    hpfB1_ = -(1.f + alpha) / 2.f;
    hpfA1_ = -alpha;
}

} // namespace hexcaster
