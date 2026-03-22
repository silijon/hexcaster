#pragma once

#include "../meter_data.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"

#include <ftxui/dom/elements.hpp>
#include <vector>

namespace hexcaster::tui {

/**
 * Bloom screen: envelope follower activity and Bloom gain parameters.
 *
 * Shows:
 *   - Envelope (observation, read-only) [0, 1]
 *   - Bloom pre-gain dB (adjustable)
 *   - Bloom post-gain dB (adjustable)
 *   - Bloom depth dB (adjustable)
 *   - Bloom compensation ratio (adjustable)
 *   - Bloom sensitivity dB (adjustable)
 */

std::vector<MeterDesc> buildBloomScreenMeters();

ftxui::Element renderBloomScreen(const MeterData&              data,
                                 const std::vector<MeterDesc>& meters,
                                 int                           selectedIdx,
                                 const ParamRegistry&          registry,
                                 const MidiMap&                midiMap);

} // namespace hexcaster::tui
