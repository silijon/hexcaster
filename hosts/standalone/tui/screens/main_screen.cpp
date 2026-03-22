#include "main_screen.h"
#include "meter_widget.h"

#include <ftxui/dom/elements.hpp>

namespace hexcaster::tui {

std::vector<MeterDesc> buildMainScreenMeters()
{
    return {
        // Gate activity: observation-only (gate gain [0,1])
        MeterDesc::fromObservation("Gate",
            [](const MeterData& d) { return d.gateGain; },
            ""),

        // Gate threshold: registry param (dB)
        MeterDesc::fromParam("Threshold", ParamId::NoiseGateThreshold_dB, " dB"),

        // Input gain: registry param (dB)
        MeterDesc::fromParam("In Gain", ParamId::InputGain_dB, " dB"),

        // Master volume: registry param (dB)
        MeterDesc::fromParam("Out Vol", ParamId::MasterVolume_dB, " dB"),
    };
}

ftxui::Element renderMainScreen(const MeterData&              data,
                                const std::vector<MeterDesc>& meters,
                                int                           selectedIdx,
                                const ParamRegistry&          registry,
                                const MidiMap&                midiMap)
{
    using namespace ftxui;

    // Gate state text row
    const char* stateLabel = gateStateStr(data.gateState);

    auto stateRow = hbox(Elements{
        text(" Gate state: "),
        text(stateLabel) | bold,
    });
    return vbox(Elements{
        text(" Main: Noise Gate + Levels") | bold,
        text(""),
        makeMeterRow(meters, selectedIdx, data, registry, midiMap),
        text(""),
        stateRow,
    });
}

} // namespace hexcaster::tui
