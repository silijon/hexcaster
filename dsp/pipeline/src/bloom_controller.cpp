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
    detectorAttackCoeff_   = msToCoeff(kDetectorAttackMs, sampleRate_);
    detectorReleaseCoeff_  = msToCoeff(kDetectorReleaseMs, sampleRate_);
    detectorSmoothCoeff_   = msToCoeff(kDetectorSmoothMs, sampleRate_);

    // Chord score smoothing
    chordScoreSmoothCoeff_ = msToCoeff(kChordScoreSmoothMs, sampleRate_);

    cachedAttackMs_        = -1.f;  // force gain envelope coefficient recompute on first block
    cachedReleaseMs_       = -1.f;

    reset();
}

void BloomController::reset()
{
    hpfX1_                 = 0.f;
    hpfY1_                 = 0.f;
    detectorRawEnv_        = 0.f;
    detectorPeak_          = 0.f;
    detectorSmoothEnv_     = 0.f;

    gainEnv_               = 0.f;

    // Chord score: pretend the last transient was a long time ago so the
    // first detected transient is treated as isolated (full slope weight).
    samplesSinceLastTransient_ = static_cast<int>(sampleRate_);  // ~1s
    lastTransientInterval_     = static_cast<int>(sampleRate_);
    chordScoreTarget_          = 0.f;
    chordScoreSmoothed_        = 0.f;
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
    const float attackMs          = attackMs_.load(std::memory_order_relaxed);
    const float releaseMs         = releaseMs_.load(std::memory_order_relaxed);
    const float sensitivityLin    = std::pow(10.f, sensitivity_.load(std::memory_order_relaxed) / 20.f);

    // Recompute EMA coefficients if attack/release changed
    if (attackMs != cachedAttackMs_ || releaseMs != cachedReleaseMs_) {
        cachedAttackMs_  = attackMs;
        cachedReleaseMs_ = releaseMs;
        updateCoefficients();
    }

    float detRawEnv        = detectorRawEnv_;
    float detSmoothEnv     = detectorSmoothEnv_;
    float detPeak          = detectorPeak_;
    float detSlope         = detectorSlope_;
    float detMaxSlope      = detectorMaxSlope_;
    bool  detUnderAttack   = detectorUnderAttack_;
    int   detHoldoffCounter = detectorHoldoffCounter_;

    int   samplesSinceLastTrans = samplesSinceLastTransient_;
    int   lastTransInterval     = lastTransientInterval_;
    float chordScoreTarget      = chordScoreTarget_;
    float chordScoreSmoothed    = chordScoreSmoothed_;
    const float fastNoteSamples = kFastNoteMs * 0.001f * sampleRate_;

    float gainEnv        = gainEnv_;

    // Run detector HPF + envelope per-sample across the block.
    // The HPF is applied to the detection signal only -- the audio buffer
    // is const and is not modified.
    for (int i = 0; i < numSamples; ++i) {
        // ---------------------------------------------------------------
        // Stage 1a: detector HPF (sidechain only, not audio path)
        // ---------------------------------------------------------------
        const float x      = buffer[i];
        const float hpfOut = hpfB0_ * x + hpfB1_ * hpfX1_ - hpfA1_ * hpfY1_;
        hpfX1_ = x;
        hpfY1_ = hpfOut;

        // ---------------------------------------------------------------
        // Stage 1b: simple fast peak detector
        // ---------------------------------------------------------------
        const float absSample = (hpfOut < 0.f ? -hpfOut : hpfOut) * sensitivityLin;
        if (absSample > detRawEnv)
            detRawEnv = detectorAttackCoeff_  * detRawEnv + (1.f - detectorAttackCoeff_)  * absSample;
        else
            detRawEnv = detectorReleaseCoeff_ * detRawEnv + (1.f - detectorReleaseCoeff_) * absSample;

        // ---------------------------------------------------------------
        // Stage 1c: smoothing LPF on detector output
        // ---------------------------------------------------------------
        const float prevSD = detSmoothEnv;
        detSmoothEnv = detectorSmoothCoeff_ * detSmoothEnv + (1.f - detectorSmoothCoeff_) * detRawEnv;

        // ---------------------------------------------------------------
        // Stage 1d: detector slope
        // ---------------------------------------------------------------
        // delta is the per-sample rate of change of the smoothed detector.
        // ---------------------------------------------------------------
        const float delta    = detSmoothEnv - prevSD;

        // ---------------------------------------------------------------
        // Stage 1e: peak and slope detection 
        // ---------------------------------------------------------------
        // find a new peak
        const float alpha = 0.005f; // how much we are considering the delta component in slope derivation 
        const float epsilon = 0.00005f; // min slope difference to discover new peak 
        detSlope += alpha * (delta - detSlope);  // calculate a smoothed slope from the raw delta
        
        // calculate the peak and the max slope for the current transient
        if (detSlope > epsilon) { // new peak
            detHoldoffCounter = kDetectorHoldoffSamples;
            if (!detUnderAttack) {
                // this is the start of a new transient
                detUnderAttack = true;
                detMaxSlope = detSlope;  // reset
                detPeak = detSmoothEnv;  // reset
                // capture the gap before this onset for dynamic weighting
                lastTransInterval = samplesSinceLastTrans;
                samplesSinceLastTrans = 0;
            }
            if (detSmoothEnv > detPeak) { // peak is rising
                detPeak = detSmoothEnv;
                detMaxSlope = std::max(detSlope, detMaxSlope);
            }
        } else {
            if (detUnderAttack) {
                // keep tracking peak and slope during holdoff
                if (detSmoothEnv > detPeak) {
                    detPeak = detSmoothEnv;
                    detMaxSlope = std::max(detSlope, detMaxSlope);
                }
                if (--detHoldoffCounter <= 0) {
                    // attack phase ended
                    // detPeak and detMaxSlope are finalized for this transient
                    // hold them until the next transient
                    detUnderAttack = false;
                    // Compute chord score target now that peak + maxSlope
                    // are finalized. Dynamic blend: long interval since
                    // previous onset → trust slope; short interval → trust
                    // peak. Trailing * kPeakRef rescales so chord-typical
                    // values land near the existing 0.5 gain pivot.
                    const float speedFactor = std::min(
                        static_cast<float>(lastTransInterval) / fastNoteSamples, 1.f);
                    const float slopeWeight = speedFactor;
                    const float peakWeight  = 1.f - speedFactor;
                    const float peakNorm    = std::min(detPeak     / kPeakRef,  1.f);
                    const float slopeNorm   = std::min(detMaxSlope / kSlopeRef, 1.f);
                    chordScoreTarget = (peakWeight * peakNorm + slopeWeight * slopeNorm) * kPeakRef;
                }
            }
        }

        // Smooth the held chord score target every sample so step changes
        // at end-of-attack don't pump the gain envelope audibly.
        chordScoreSmoothed = chordScoreSmoothCoeff_ * chordScoreSmoothed
                           + (1.f - chordScoreSmoothCoeff_) * chordScoreTarget;
        ++samplesSinceLastTrans;
    }

    detectorRawEnv_        = detRawEnv;
    detectorSmoothEnv_     = detSmoothEnv;
    detectorPeak_          = detPeak;
    detectorSlope_         = detSlope;
    detectorMaxSlope_      = detMaxSlope;
    detectorUnderAttack_   = detUnderAttack;
    detectorHoldoffCounter_ = detHoldoffCounter;

    samplesSinceLastTransient_ = samplesSinceLastTrans;
    lastTransientInterval_     = lastTransInterval;
    chordScoreTarget_          = chordScoreTarget;
    chordScoreSmoothed_        = chordScoreSmoothed;

#if HEXCASTER_EXPERIMENTAL_BLOOM
    // Experimental chord-aware power curve.
    const float gainCoefficient = std::pow(2.f, gainEnvReleaseShape_);
    const float chordScoreFloored = std::max(chordScoreSmoothed, kChordFloor);
    gainEnv = std::pow(
        detSmoothEnv,
        1.f / (gainCoefficient * std::pow(chordScoreFloored, gainEnvReleaseShape_)));
#else
    // Default envelope follower: attack when the detector rises above the
    // current gain envelope, release when it falls below it.
    const float coefficient = detSmoothEnv > gainEnv
        ? gainEnvAttackCoeff_
        : gainEnvReleaseCoeff_;
    gainEnv = coefficient * gainEnv + (1.f - coefficient) * detSmoothEnv;
#endif
    gainEnv_ = gainEnv;

    // Publish observations for visual output.
    // Det Env: smooth envelope is the operative detection envelope.
    // Gain Env: always the gain envelope.
    const float detRawForViz = std::clamp(detRawEnv, 0.f, 1.f);
    observedDetectorRawEnvelope_.store(detRawForViz, std::memory_order_relaxed);

    const float detForViz = std::clamp(detSmoothEnv, 0.f, 1.f);
    observedDetectorEnvelope_.store(detForViz, std::memory_order_relaxed);

    const float peakForViz = std::clamp(detPeak, 0.f, 1.f);
    observedDetectorPeak_.store(peakForViz, std::memory_order_relaxed);

    const float slopeForViz = std::clamp(detMaxSlope * 1000, -1.f, 1.f);
    observedDetectorSlope_.store(slopeForViz, std::memory_order_relaxed);

    // Clamp for the gain formulas (safety net -- gainEnv should already be [0,1]).
    const float clampedGainEnv = std::clamp(gainEnv, 0.f, 1.f);
    observedGainEnvelope_.store(clampedGainEnv, std::memory_order_relaxed);

    const float chordScoreForViz = std::clamp(chordScoreSmoothed, 0.f, 1.f);
    observedChordScore_.store(chordScoreForViz, std::memory_order_relaxed);

    // Compute gain targets from the envelope
    const float reductionDb = depth * clampedGainEnv;
    const float preGainDb   = basePreDb  - reductionDb;
    // TODO: can we automate compensation?
    const float postGainDb  = basePostDb + compensation * reductionDb;
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

float BloomController::getDetectorEnvelope() const
{
    return observedDetectorEnvelope_.load(std::memory_order_relaxed);
}

float BloomController::getDetectorRawEnvelope() const
{
    return observedDetectorRawEnvelope_.load(std::memory_order_relaxed);
}

float BloomController::getDetectorPeak() const
{
    return observedDetectorPeak_.load(std::memory_order_relaxed);
}

float BloomController::getDetectorSlope() const
{
    return observedDetectorSlope_.load(std::memory_order_relaxed);
}

float BloomController::getGainEnvelope() const
{
    return observedGainEnvelope_.load(std::memory_order_relaxed);
}

float BloomController::getChordScore() const
{
    return observedChordScore_.load(std::memory_order_relaxed);
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
    depth_.store(std::clamp(db, 0.f, 32.f), std::memory_order_relaxed);
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
    releaseMs_.store(std::clamp(ms, 0.5f, 10.f), std::memory_order_relaxed);
}

void BloomController::setSensitivity(float db)
{
    sensitivity_.store(std::clamp(db, 0.f, 20.f), std::memory_order_relaxed);
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
    gainEnvAttackCoeff_  = msToCoeff(cachedAttackMs_, sampleRate_);
    gainEnvReleaseCoeff_ = msToCoeff(cachedReleaseMs_, sampleRate_);
    gainEnvReleaseShape_ = cachedReleaseMs_;
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
