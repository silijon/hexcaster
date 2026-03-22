#include "bloom_screen.h"
#include "meter_widget.h"

#include <ftxui/dom/elements.hpp>

namespace hexcaster::tui {

std::vector<MeterDesc> buildBloomScreenMeters()
{
    // Applied pre/post gain range: matches GainStage clamp limits [-60, +24] dB.
    // This covers every value the BloomController can ever write to these stages.
    constexpr float kGainMin = -60.f;
    constexpr float kGainMax =  24.f;

    return {
        // Envelope: observation-only, [0,1]
        MeterDesc::fromObservation("Envelope",
            [](const MeterData& d) { return d.bloomEnvelope; },
            ""),

        // Applied pre/post gain: what the BloomController actually set last block.
        // Falls as envelope rises (pre) and rises (post) -- shows dynamic action.
        MeterDesc::fromObservationRanged("Pre dB",
            [](const MeterData& d) { return d.bloomPreGainApplied; },
            kGainMin, kGainMax, " dB"),

        MeterDesc::fromObservationRanged("Post dB",
            [](const MeterData& d) { return d.bloomPostGainApplied; },
            kGainMin, kGainMax, " dB"),

        // Tuning parameters (adjustable unless MIDI-mapped)
        MeterDesc::fromParam("Depth",     ParamId::BloomDepth,          " dB"),
        MeterDesc::fromParam("Comp",      ParamId::BloomCompensation,   ""),
        MeterDesc::fromParam("Sensitiv.", ParamId::BloomSensitivity_dB, " dB"),
        MeterDesc::fromParam("Env Atk",  ParamId::EnvAttackMs,          " ms"),
        MeterDesc::fromParam("Env Rel",  ParamId::EnvReleaseMs,          " ms"),
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
