#pragma once

#include <cstdint>
#include "hexcaster/param_id.h"

namespace hexcaster {

class ParamRegistry;

/**
 * MidiMap: maps MIDI CC numbers to ParamIds.
 *
 * Provides a simple lookup table from CC number [0, 127] to a ParamId.
 * The host calls dispatch() when a CC message arrives; MidiMap translates
 * and writes to the ParamRegistry.
 *
 * Intended for:
 *   - Hardware controller knobs/pedals
 *   - DAW automation
 *   - Future preset recall via PC messages
 *
 * Implementation: stub. To be expanded in Phase 4.
 */
class MidiMap {
public:
    static constexpr int kNumCCs = 128;
    static constexpr uint8_t kUnmapped = 0xFF;

    MidiMap();

    /**
     * Assign a CC number to a parameter.
     * ccNumber: [0, 127]
     * id: target parameter
     */
    void map(uint8_t ccNumber, ParamId id);

    /**
     * Remove the mapping for a CC number.
     */
    void unmap(uint8_t ccNumber);

    /**
     * Dispatch an incoming CC message to the registry.
     * value: [0, 127] raw MIDI value, normalized to [0,1] range internally.
     * Returns true if the CC was mapped and dispatched.
     *
     * Not real-time safe in current stub form (may be made RT-safe later).
     */
    bool dispatch(uint8_t ccNumber, uint8_t value, ParamRegistry& registry);

private:
    // Store ParamId as uint32_t; kUnmapped sentinel stored as 0xFFFFFFFF
    uint32_t mappings_[kNumCCs];
};

} // namespace hexcaster
