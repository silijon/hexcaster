#include "hexcaster/midi_map.h"
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

    // Scale raw MIDI [0, 127] linearly to the parameter's [min, max] range.
    const auto range = registry.getRange(id);
    const float t = static_cast<float>(value) / 127.f;
    const float paramValue = range.min + t * (range.max - range.min);

    registry.set(id, paramValue);
    return true;
}

} // namespace hexcaster
