#include "hexcaster/noise_gate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hexcaster {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Convert a time constant in milliseconds to an EMA coefficient.
// coeff close to 1.0 = slow (long time constant)
// coeff close to 0.0 = fast (short time constant)
// Uses a one-pole filter: tau = -1 / (ln(coeff) * sampleRate)
// Solved for coeff: coeff = exp(-1 / (ms/1000 * sampleRate))
float NoiseGate::msToCoeff(float ms, float sampleRate)
{
    if (ms <= 0.f || sampleRate <= 0.f) return 0.f;
    return std::exp(-1.f / (ms * 0.001f * sampleRate));
}

float NoiseGate::dbToLinear(float db)
{
    return std::pow(10.f, db / 20.f);
}

// ---------------------------------------------------------------------------
// ProcessorStage interface
// ---------------------------------------------------------------------------

void NoiseGate::prepare(float sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    reset();
    cachedThresholdDb_ = std::numeric_limits<float>::quiet_NaN();
    cachedAttackMs_ = std::numeric_limits<float>::quiet_NaN();
    cachedReleaseMs_ = std::numeric_limits<float>::quiet_NaN();
    cachedHoldMs_ = std::numeric_limits<float>::quiet_NaN();
    updateCoefficients();
}

void NoiseGate::reset()
{
    state_       = State::Closed;
    envelope_    = 0.f;
    gateGain_    = 0.f;
    holdCounter_ = 0;
}

void NoiseGate::process(float* buffer, int numSamples)
{
    // Atomics are read once per block; transcendentals run only on control
    // changes.
    updateCoefficients();

    for (int i = 0; i < numSamples; ++i) {
        const float input = buffer[i];
        const float absSample = input < 0.f ? -input : input;

        // --- Envelope follower ---
        // Fast attack: snap up to peaks immediately.
        // Slow release: decay governed by envReleaseCoeff_.
        if (absSample > envelope_) {
            envelope_ = absSample;
        } else {
            envelope_ = envReleaseCoeff_ * envelope_;
        }

        // --- State machine ---
        switch (state_) {
            case State::Closed:
                if (envelope_ >= thresholdLin_) {
                    state_ = State::Opening;
                    holdCounter_ = holdSamples_;
                }
                break;

            case State::Opening:
                // Ramp gain toward 1.0 using attack coefficient
                gateGain_ = attackCoeff_ * gateGain_ + (1.f - attackCoeff_) * 1.f;
                if (gateGain_ >= 0.999f) {
                    gateGain_ = 1.f;
                    state_ = State::Open;
                }
                break;

            case State::Open:
                if (envelope_ < thresholdLin_) {
                    state_ = State::Holding;
                    holdCounter_ = holdSamples_;
                }
                break;

            case State::Holding:
                if (envelope_ >= thresholdLin_) {
                    // Signal came back up during hold -- stay open
                    state_ = State::Open;
                    holdCounter_ = holdSamples_;
                } else if (--holdCounter_ <= 0) {
                    state_ = State::Closing;
                }
                break;

            case State::Closing:
                // Ramp gain toward 0.0 using release coefficient
                gateGain_ = releaseCoeff_ * gateGain_;
                if (gateGain_ <= 0.001f) {
                    gateGain_ = 0.f;
                    state_ = State::Closed;
                }
                // If signal returns above threshold while closing, re-open
                if (envelope_ >= thresholdLin_) {
                    state_ = State::Opening;
                    holdCounter_ = holdSamples_;
                }
                break;
        }

        buffer[i] = input * gateGain_;
    }

    if (observationEnabled_.load(std::memory_order_relaxed)) {
        observedGateGain_.store(gateGain_, std::memory_order_relaxed);
        observedState_.store(static_cast<uint8_t>(state_), std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Parameter setters / getters
// ---------------------------------------------------------------------------

void NoiseGate::setThresholdDb(float db)
{
    thresholdDb_.store(std::clamp(db, -80.f, 0.f), std::memory_order_relaxed);
}

void NoiseGate::setAttackMs(float ms)
{
    attackMs_.store(std::clamp(ms, 0.1f, 10.f), std::memory_order_relaxed);
}

void NoiseGate::setReleaseMs(float ms)
{
    releaseMs_.store(std::clamp(ms, 5.f, 500.f), std::memory_order_relaxed);
}

void NoiseGate::setHoldMs(float ms)
{
    holdMs_.store(std::clamp(ms, 0.f, 500.f), std::memory_order_relaxed);
}

void NoiseGate::setObservationEnabled(bool enabled)
{
    observationEnabled_.store(enabled, std::memory_order_relaxed);
}

float NoiseGate::getThresholdDb()  const { return thresholdDb_.load(std::memory_order_relaxed); }
float NoiseGate::getAttackMs()     const { return attackMs_.load(std::memory_order_relaxed); }
float NoiseGate::getReleaseMs()    const { return releaseMs_.load(std::memory_order_relaxed); }
float NoiseGate::getHoldMs()       const { return holdMs_.load(std::memory_order_relaxed); }

float NoiseGate::getGateGain() const
{
    return observedGateGain_.load(std::memory_order_relaxed);
}

NoiseGate::State NoiseGate::getState() const
{
    return static_cast<State>(observedState_.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void NoiseGate::updateCoefficients()
{
    const float thresholdDb = thresholdDb_.load(std::memory_order_relaxed);
    const float attackMs = attackMs_.load(std::memory_order_relaxed);
    const float releaseMs = releaseMs_.load(std::memory_order_relaxed);
    const float holdMs = holdMs_.load(std::memory_order_relaxed);
    if (thresholdDb == cachedThresholdDb_ && attackMs == cachedAttackMs_
        && releaseMs == cachedReleaseMs_ && holdMs == cachedHoldMs_)
        return;

    cachedThresholdDb_ = thresholdDb;
    cachedAttackMs_ = attackMs;
    cachedReleaseMs_ = releaseMs;
    cachedHoldMs_ = holdMs;
    thresholdLin_    = dbToLinear(thresholdDb);
    attackCoeff_     = msToCoeff(attackMs, sampleRate_);
    releaseCoeff_    = msToCoeff(releaseMs, sampleRate_);
    // Envelope follower release: ~3x faster than gate release for responsiveness
    envReleaseCoeff_ = msToCoeff(releaseMs / 3.f, sampleRate_);
    holdSamples_     = static_cast<int>(holdMs * 0.001f * sampleRate_);
}

} // namespace hexcaster
