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
    detectorAttackCoeff_  = msToCoeff(kDetectorAttackMs,  sampleRate_);
    detectorReleaseCoeff_ = msToCoeff(kDetectorReleaseMs, sampleRate_);

    cachedAttackMs_  = -1.f;  // force gain envelope coefficient recompute on first block
    cachedReleaseMs_ = -1.f;
    reset();
}

void BloomController::reset()
{
    detectorEnv_ = 0.f;
    gainEnv_     = 0.f;
    hpfX1_       = 0.f;
    hpfY1_       = 0.f;
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
    float detEnv  = detectorEnv_;
    float gainEnv = gainEnv_;

    for (int i = 0; i < numSamples; ++i) {
        // Stage 1a: detector HPF (sidechain only, not audio path)
        const float x      = buffer[i];
        const float hpfOut = hpfB0_ * x + hpfB1_ * hpfX1_ - hpfA1_ * hpfY1_;
        hpfX1_ = x;
        hpfY1_ = hpfOut;

        // Stage 1b: fast peak detector.
        // Sensitivity scales the detection signal to normalise raw ADC amplitude.
        // Uses fixed short attack/release so it responds near-instantaneously to
        // note onsets -- the detector answers "is there signal?"
        const float absSample = (hpfOut < 0.f ? -hpfOut : hpfOut) * sensitivityLin;
        if (absSample > detEnv)
            detEnv = detectorAttackCoeff_  * detEnv + (1.f - detectorAttackCoeff_)  * absSample;
        else
            detEnv = detectorReleaseCoeff_ * detEnv + (1.f - detectorReleaseCoeff_) * absSample;

        // Stage 2: gain envelope (user attack/release shape the gain response).
        // Attack: ramp toward the detector value at the user attack rate.
        // Release: decay toward zero at the user release rate -- completely
        //   independent of the detector's waveform. This gives a clean
        //   exponential decay contour rather than tracking the audio envelope.
        if (detEnv > gainEnv)
            gainEnv = gainAttackCoeff_  * gainEnv + (1.f - gainAttackCoeff_)  * detEnv;
        else
            gainEnv = gainReleaseCoeff_ * gainEnv;
    }

    detectorEnv_ = detEnv;
    gainEnv_     = gainEnv;

    // Publish both envelopes for TUI observation (relaxed -- no ordering required).
    // detEnv: fast audio tracker -- shows what the signal is doing.
    // gainEnv: gain-driving envelope -- shows what bloom is actually doing.
    observedDetectorEnv_.store(std::clamp(detEnv,  0.f, 1.f), std::memory_order_relaxed);
    observedEnvelope_.store   (std::clamp(gainEnv, 0.f, 1.f), std::memory_order_relaxed);

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
