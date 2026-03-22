#pragma once

#include "../meter_data.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"

#include <ftxui/dom/elements.hpp>
#include <vector>

namespace hexcaster::tui {

/**
 * EQ screen: mid-sweep EQ gain, center frequency, and Q factor.
 */

std::vector<MeterDesc> buildEqScreenMeters();

ftxui::Element renderEqScreen(const MeterData&              data,
                              const std::vector<MeterDesc>& meters,
                              int                           selectedIdx,
                              const ParamRegistry&          registry,
                              const MidiMap&                midiMap);

} // namespace hexcaster::tui
