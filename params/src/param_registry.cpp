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
    info[idx(ParamId::BloomPreDepth)]    = { 6.f,    0.f, 24.f };
    info[idx(ParamId::BloomPostDepth)]   = { 3.f,    0.f, 24.f };
    info[idx(ParamId::EnvAttackMs)]      = { 5.f,    0.1f, 500.f };
    info[idx(ParamId::EnvReleaseMs)]     = {100.f,   1.f, 5000.f };

    // Post-EQ
    info[idx(ParamId::EqBand1Freq)]      = { 100.f,  20.f, 20000.f };
    info[idx(ParamId::EqBand1GainDb)]    = {   0.f, -24.f,    24.f };
    info[idx(ParamId::EqBand1Q)]         = {   1.f,  0.1f,    10.f };
    info[idx(ParamId::EqBand2Freq)]      = {1000.f,  20.f, 20000.f };
    info[idx(ParamId::EqBand2GainDb)]    = {   0.f, -24.f,    24.f };
    info[idx(ParamId::EqBand2Q)]         = {   1.f,  0.1f,    10.f };
    info[idx(ParamId::EqBand3Freq)]      = {8000.f,  20.f, 20000.f };
    info[idx(ParamId::EqBand3GainDb)]    = {   0.f, -24.f,    24.f };
    info[idx(ParamId::EqBand3Q)]         = {   1.f,  0.1f,    10.f };

    // Master
    info[idx(ParamId::MasterGain_dB)]    = {   0.f, -60.f,   24.f };

    // Reverb
    info[idx(ParamId::ReverbRoomSize)]   = { 0.5f,   0.f,    1.f };
    info[idx(ParamId::ReverbDamping)]    = { 0.5f,   0.f,    1.f };
    info[idx(ParamId::ReverbWet_Norm)]   = { 0.0f,   0.f,    1.f };

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

} // namespace hexcaster
