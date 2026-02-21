#pragma once

namespace hexcaster {

/**
 * ParamSmoother: exponential moving average smoother for parameter values.
 *
 * Used in the audio thread to smoothly interpolate toward a target
 * parameter value, avoiding clicks/zippering on control changes.
 *
 * Real-time safe: no allocation, no branching on hot path.
 *
 * Usage:
 *   ParamSmoother smoother;
 *   smoother.prepare(48000.f, 20.f);   // 20ms smoothing time
 *   smoother.setTarget(0.5f);
 *   float val = smoother.next();        // call once per sample
 */
class ParamSmoother {
public:
    ParamSmoother() = default;

    /**
     * Configure the smoother. Not real-time safe.
     *
     * @param sampleRate      Audio sample rate in Hz.
     * @param smoothingMs     Smoothing time constant in milliseconds.
     *                        Higher = slower transitions. Typical: 5-50ms.
     */
    void prepare(float sampleRate, float smoothingMs = 20.f);

    /**
     * Set the target value. Thread-safe (atomic write by convention --
     * call from control thread, read via next() from audio thread).
     * Typically set once per block, then next() called once per sample.
     */
    void setTarget(float target);

    /**
     * Advance by one sample and return the smoothed value.
     * Real-time safe.
     */
    float next();

    /**
     * Snap immediately to the target (no smoothing).
     * Use after prepare() or reset().
     */
    void snap(float value);

    float getCurrentValue() const { return current_; }
    float getTargetValue()  const { return target_;  }

private:
    float current_  = 0.f;
    float target_   = 0.f;
    float coeff_    = 0.f;   // EMA coefficient (1 - alpha)
};

} // namespace hexcaster
