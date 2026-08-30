#include "hexcaster/midi_map.h"
#include "hexcaster/input_gain.h"
#include "hexcaster/param_registry.h"

#include <cstring>
#include <limits>

namespace hexcaster {

static constexpr uint32_t kSentinel = 0xFFFFFFFF;

MidiMap::MidiMap()
{
    for (auto& m : mappings_)
        m = kSentinel;
}

void MidiMap::map(uint8_t ccNumber, ParamId id)
{
    if (ccNumber >= kNumCCs) return;
    mappings_[ccNumber] = static_cast<uint32_t>(id);
}

void MidiMap::unmap(uint8_t ccNumber)
{
    if (ccNumber >= kNumCCs) return;
    mappings_[ccNumber] = kSentinel;
}

bool MidiMap::dispatch(uint8_t ccNumber, uint8_t value, ParamRegistry& registry)
{
    if (ccNumber >= kNumCCs) return false;

    const uint32_t raw = mappings_[ccNumber];
    if (raw == kSentinel) return false;

    const ParamId id = static_cast<ParamId>(raw);

    const auto range = registry.getRange(id);
    const bool centeredAtDefault =
        id == ParamId::InputGain_dB ||
        id == ParamId::HighShelfGain_dB ||
        id == ParamId::LowShelfGain_dB;
    const float paramValue = centeredAtDefault
        ? centeredValueFromMidiCc(value, range.min,
                                  ParamRegistry::getDefault(id), range.max)
        : range.min + (static_cast<float>(value) / 127.f) * (range.max - range.min);

    registry.set(id, paramValue);
    return true;
}

bool MidiMap::isMapped(ParamId id) const
{
    const uint32_t target = static_cast<uint32_t>(id);
    for (int i = 0; i < kNumCCs; ++i) {
        if (mappings_[i] == target) return true;
    }
    return false;
}

} // namespace hexcaster
