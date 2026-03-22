#pragma once

#include "../meter_data.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"

#include <ftxui/dom/elements.hpp>
#include <vector>

namespace hexcaster::tui {

/**
 * Main screen: noise gate, input gain, master volume.
 *
 * Shows:
 *   - Gate activity meter (gate gain observation, read-only)
 *   - Noise gate threshold (adjustable)
 *   - Input gain (adjustable)
 *   - Master volume (adjustable)
 */

/** Returns the meter descriptors for the main screen. */
std::vector<MeterDesc> buildMainScreenMeters();

/** Render the main screen content element. */
ftxui::Element renderMainScreen(const MeterData&              data,
                                const std::vector<MeterDesc>& meters,
                                int                           selectedIdx,
                                const ParamRegistry&          registry,
                                const MidiMap&                midiMap);

} // namespace hexcaster::tui
