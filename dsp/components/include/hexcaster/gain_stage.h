#pragma once

#include <atomic>
#include "hexcaster/processor_stage.h"
#include "hexcaster/param_smoother.h"

namespace hexcaster {

/**
 * GainStage: applies a smoothed linear gain to an audio buffer.
 *
 * - Gain is specified in dB externally; stored as linear internally.
 * - Transitions are smoothed per-sample to avoid clicks.
 * - Safe limits are clamped at set time.
 * - No dynamic allocation. No denormals (gain floor enforced).
 *
 * Usage:
 *   GainStage gain;
 *   gain.prepare(48000.f, 128);
 *   gain.setGainDb(0.f);           // unity
 *   gain.process(buffer, 128);
 */
class GainStage : public ProcessorStage {
public:
    static constexpr float kMinDb  = -60.f;
    static constexpr float kMaxDb  =  24.f;
    static constexpr float kMinLin =  1e-3f; // floor to avoid denormals

    GainStage();

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    /**
     * Set target gain in dB. Clamped to [kMinDb, kMaxDb].
     * Thread-safe: may be called from control thread.
     */
    void setGainDb(float db);

    /**
     * Set target gain as a linear multiplier directly.
     * Thread-safe: may be called from control thread.
     */
    void setGainLinear(float linear);

    float getGainDb() const;
    float getGainLinear() const;

private:
    std::atomic<float> targetGainLinear_;
    ParamSmoother      smoother_;
};

} // namespace hexcaster
