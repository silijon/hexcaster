#include "eq_screen.h"
#include "meter_widget.h"

#include <ftxui/dom/elements.hpp>

namespace hexcaster::tui {

std::vector<MeterDesc> buildEqScreenMeters()
{
    return {
        MeterDesc::fromParam("Lo Shelf Gain", ParamId::LowShelfGain_dB,  " dB"),
        MeterDesc::fromParam("Hi Shelf Gain", ParamId::HighShelfGain_dB, " dB"),
    };
}

ftxui::Element renderEqScreen(const MeterData&              data,
                              const std::vector<MeterDesc>& meters,
                              int                           selectedIdx,
                              const ParamRegistry&          registry,
                              const MidiMap&                midiMap)
{
    using namespace ftxui;
    (void)data;

    auto infoRow = hbox(Elements{
        text(" Range: Gain [-32, +12] dB  |  Lo shelf @ 175 Hz  |  Hi shelf @ 4500 Hz") | dim,
    });
    return vbox(Elements{
        text(" EQ: High + Low Shelf") | bold,
        text(""),
        makeMeterRow(meters, selectedIdx, data, registry, midiMap),
        text(""),
        infoRow,
    });
}

} // namespace hexcaster::tui
