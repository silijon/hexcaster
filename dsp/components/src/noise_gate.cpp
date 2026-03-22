#include "hexcaster/noise_gate.h"

#include <algorithm>
#include <cmath>

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
    // Refresh coefficients once per block from the atomics.
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

    // Publish observation values for TUI polling (relaxed -- just visibility).
    observedGateGain_.store(gateGain_, std::memory_order_relaxed);
    observedState_.store(static_cast<uint8_t>(state_), std::memory_order_relaxed);
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
    thresholdLin_    = dbToLinear(thresholdDb_.load(std::memory_order_relaxed));
    attackCoeff_     = msToCoeff(attackMs_.load(std::memory_order_relaxed),   sampleRate_);
    releaseCoeff_    = msToCoeff(releaseMs_.load(std::memory_order_relaxed),  sampleRate_);
    // Envelope follower release: ~3x faster than gate release for responsiveness
    envReleaseCoeff_ = msToCoeff(releaseMs_.load(std::memory_order_relaxed) / 3.f, sampleRate_);
    holdSamples_     = static_cast<int>(holdMs_.load(std::memory_order_relaxed)
                                        * 0.001f * sampleRate_);
}

} // namespace hexcaster
