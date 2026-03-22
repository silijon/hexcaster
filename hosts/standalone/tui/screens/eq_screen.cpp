#include "eq_screen.h"
#include "meter_widget.h"

#include <ftxui/dom/elements.hpp>

namespace hexcaster::tui {

std::vector<MeterDesc> buildEqScreenMeters()
{
    return {
        MeterDesc::fromParam("EQ Gain",  ParamId::EqGain_dB, " dB"),
        MeterDesc::fromParam("EQ Sweep", ParamId::EqSweepHz, " Hz"),
        MeterDesc::fromParam("EQ Q",     ParamId::EqQ,       ""),
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
        text(" Range: Gain +/-12 dB  |  Sweep 300-2500 Hz  |  Q 0.3-3.0") | dim,
    });
    return vbox(Elements{
        text(" EQ: Mid-Sweep Peaking Filter") | bold,
        text(""),
        makeMeterRow(meters, selectedIdx, data, registry, midiMap),
        text(""),
        infoRow,
    });
}

} // namespace hexcaster::tui
