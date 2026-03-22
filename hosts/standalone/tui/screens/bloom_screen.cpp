#include "bloom_screen.h"
#include "meter_widget.h"

#include <ftxui/dom/elements.hpp>

namespace hexcaster::tui {

std::vector<MeterDesc> buildBloomScreenMeters()
{
    return {
        // Envelope: observation-only, [0,1]
        MeterDesc::fromObservation("Envelope",
            [](const MeterData& d) { return d.bloomEnvelope; },
            ""),

        // Bloom parameters (all adjustable unless MIDI-mapped)
        MeterDesc::fromParam("Pre Gain",  ParamId::BloomBasePre_dB,    " dB"),
        MeterDesc::fromParam("Post Gain", ParamId::BloomBasePost_dB,   " dB"),
        MeterDesc::fromParam("Depth",     ParamId::BloomDepth,         " dB"),
        MeterDesc::fromParam("Comp",      ParamId::BloomCompensation,  ""),
        MeterDesc::fromParam("Sensitiv.", ParamId::BloomSensitivity_dB," dB"),
    };
}

ftxui::Element renderBloomScreen(const MeterData&              data,
                                 const std::vector<MeterDesc>& meters,
                                 int                           selectedIdx,
                                 const ParamRegistry&          registry,
                                 const MidiMap&                midiMap)
{
    using namespace ftxui;
    (void)data;

    auto infoRow = hbox(Elements{
        text(" Envelope drives pre-gain reduction and post-gain compensation") | dim,
    });
    return vbox(Elements{
        text(" Bloom: Dynamic Gain Control") | bold,
        text(""),
        makeMeterRow(meters, selectedIdx, data, registry, midiMap),
        text(""),
        infoRow,
    });
}

} // namespace hexcaster::tui
