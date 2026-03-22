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
    cachedAttackMs_  = -1.f;  // force coefficient recompute on first block
    cachedReleaseMs_ = -1.f;
    reset();
}

void BloomController::reset()
{
    envelope_ = 0.f;
    hpfX1_    = 0.f;
    hpfY1_    = 0.f;
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

    // Run detector HPF + envelope follower per-sample across the block.
    // The HPF is applied to the detection signal only -- the audio buffer
    // is const and is not modified.
    float env = envelope_;

    for (int i = 0; i < numSamples; ++i) {
        // 1st-order high-pass filter (detector sidechain)
        const float x = buffer[i];
        const float hpfOut = hpfB0_ * x + hpfB1_ * hpfX1_ - hpfA1_ * hpfY1_;
        hpfX1_ = x;
        hpfY1_ = hpfOut;

        // Peak envelope follower -- sensitivity scales the detection signal
        // only, not the audio path. This normalises the raw ADC amplitude
        // to a useful [0, 1] working range for the gain formulas.
        const float absSample = (hpfOut < 0.f ? -hpfOut : hpfOut) * sensitivityLin;
        if (absSample > env) {
            // Attack: snap toward peak (fast EMA)
            env = attackCoeff_ * env + (1.f - attackCoeff_) * absSample;
        } else {
            // Release: decay toward current signal level (not toward zero).
            // This tracks musical dynamics -- the envelope follows the signal
            // downward at the release rate rather than free-falling to silence.
            env = releaseCoeff_ * env + (1.f - releaseCoeff_) * absSample;
        }
    }

    envelope_ = env;

    // Clamp envelope to [0, 1] for the gain formulas.
    // In practice the peak follower output tracks the signal amplitude,
    // which for a normalised guitar signal is already in this range.
    // The clamp is a safety net.
    const float clampedEnv = std::clamp(env, 0.f, 1.f);

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
    attackCoeff_  = msToCoeff(cachedAttackMs_,  sampleRate_);
    releaseCoeff_ = msToCoeff(cachedReleaseMs_, sampleRate_);
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
