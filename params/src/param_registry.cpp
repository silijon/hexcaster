#include "hexcaster/param_registry.h"
#include <algorithm>

namespace hexcaster {

// Static parameter metadata: {defaultValue, minValue, maxValue}
// Index must match ParamId enum values.
const std::array<ParamRegistry::ParamInfo, ParamRegistry::kNumParams>
ParamRegistry::kParamInfo = []() {
    std::array<ParamInfo, kNumParams> info{};

    // Initialise all to a safe default
    for (auto& p : info) {
        p = {0.f, -1e9f, 1e9f};
    }

    auto idx = [](ParamId id) { return static_cast<int>(id); };

    // Bloom
    info[idx(ParamId::BloomBasePre_dB)]  = { 0.f,  -24.f, 24.f };
    info[idx(ParamId::BloomBasePost_dB)] = { 0.f,  -24.f, 24.f };
    info[idx(ParamId::BloomDepth)]        = { 6.f,    0.f, 24.f };
    info[idx(ParamId::BloomCompensation)]= { 0.5f,   0.f,  2.f };
    info[idx(ParamId::EnvAttackMs)]          = {  5.f,    0.1f, 500.f  };
    info[idx(ParamId::EnvReleaseMs)]         = {100.f,   1.f, 5000.f  };
    info[idx(ParamId::BloomSensitivity_dB)]  = { 20.f,   0.f,   40.f };

    // Input Gain
    info[idx(ParamId::InputGain_dB)]     = {   0.f, -60.f,   24.f };

    // Noise Gate
    info[idx(ParamId::NoiseGateThreshold_dB)] = { -60.f, -80.f,   0.f };
    info[idx(ParamId::NoiseGateAttackMs)]     = {   0.5f,  0.1f,  10.f };
    info[idx(ParamId::NoiseGateReleaseMs)]    = {  50.f,   5.f,  500.f };
    info[idx(ParamId::NoiseGateHoldMs)]       = {  50.f,   0.f,  500.f };

    // Mid-Sweep EQ
    info[idx(ParamId::EqGain_dB)]  = {    0.f, -12.f,  12.f };
    info[idx(ParamId::EqSweepHz)]  = { 1000.f, 300.f, 2500.f };
    info[idx(ParamId::EqQ)]        = {    0.8f,  0.3f,   3.f };

    // Master Volume
    info[idx(ParamId::MasterVolume_dB)] = { 0.f, -60.f, 24.f };

    return info;
}();

ParamRegistry::ParamRegistry()
{
    resetToDefaults();
}

void ParamRegistry::set(ParamId id, float value)
{
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kNumParams) return;

    const auto& info = kParamInfo[i];
    const float clamped = std::clamp(value, info.minValue, info.maxValue);
    values_[i].store(clamped, std::memory_order_relaxed);
}

float ParamRegistry::get(ParamId id) const
{
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kNumParams) return 0.f;
    return values_[i].load(std::memory_order_relaxed);
}

void ParamRegistry::resetToDefaults()
{
    for (int i = 0; i < kNumParams; ++i) {
        values_[i].store(kParamInfo[i].defaultValue, std::memory_order_relaxed);
    }
}

ParamRegistry::Range ParamRegistry::getRange(ParamId id) const
{
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kNumParams) return {0.f, 1.f};
    return {kParamInfo[i].minValue, kParamInfo[i].maxValue};
}

} // namespace hexcaster
