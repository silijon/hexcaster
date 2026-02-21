#include "hexcaster/gain_stage.h"
#include <cmath>
#include <algorithm>

namespace hexcaster {

static constexpr float kSmoothingMs = 10.f;

static float dbToLinear(float db)
{
    return std::pow(10.f, db / 20.f);
}

static float linearToDb(float linear)
{
    if (linear <= 0.f) return GainStage::kMinDb;
    return 20.f * std::log10(linear);
}

GainStage::GainStage()
{
    targetGainLinear_.store(1.f, std::memory_order_relaxed);
}

void GainStage::prepare(float sampleRate, int /*maxBlockSize*/)
{
    smoother_.prepare(sampleRate, kSmoothingMs);
    smoother_.snap(targetGainLinear_.load(std::memory_order_relaxed));
}

void GainStage::process(float* buffer, int numSamples)
{
    const float target = targetGainLinear_.load(std::memory_order_relaxed);
    smoother_.setTarget(target);

    for (int i = 0; i < numSamples; ++i) {
        buffer[i] *= smoother_.next();
    }
}

void GainStage::reset()
{
    smoother_.snap(targetGainLinear_.load(std::memory_order_relaxed));
}

void GainStage::setGainDb(float db)
{
    const float clamped = std::clamp(db, kMinDb, kMaxDb);
    const float linear  = std::max(dbToLinear(clamped), kMinLin);
    targetGainLinear_.store(linear, std::memory_order_relaxed);
}

void GainStage::setGainLinear(float linear)
{
    const float clamped = std::max(linear, kMinLin);
    targetGainLinear_.store(clamped, std::memory_order_relaxed);
}

float GainStage::getGainDb() const
{
    return linearToDb(targetGainLinear_.load(std::memory_order_relaxed));
}

float GainStage::getGainLinear() const
{
    return targetGainLinear_.load(std::memory_order_relaxed);
}

} // namespace hexcaster
