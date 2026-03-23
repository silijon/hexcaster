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
        // Detector envelope: fast audio tracker (fixed ~0.1ms attack, ~10ms release)
        MeterDesc::fromObservation("Det Env",
            [](const MeterData& d) { return d.bloomDetectorEnv; },
            ""),

        // Gain envelope: what actually drives the bloom gains (user attack/release)
        MeterDesc::fromObservation("Gain Env",
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
        MeterDesc::fromParam("Depth",     ParamId::BloomDepth_dB,        " dB"),
        MeterDesc::fromParam("Comp",      ParamId::BloomCompensation,   ""),
        MeterDesc::fromParam("Sensitiv.", ParamId::BloomSensitivity_dB, " dB"),
        MeterDesc::fromParam("Atk",      ParamId::BloomAttackMs,        " ms"),
        MeterDesc::fromParam("Rel",      ParamId::BloomReleaseMs,       " ms"),
    };
}

ftxui::Element renderBloomScreen(const MeterData&              data,
                                 const std::vector<MeterDesc>& meters,
                                 int                           selectedIdx,
                                 const ParamRegistry&          registry,
                                 const MidiMap&                midiMap)
{
    using namespace ftxui;

    const char* modeName = (data.bloomMode == 1) ? "Tracking" : "Shaped";

    auto titleRow = hbox(Elements{
        text(" Bloom: Dynamic Gain Control") | bold,
        text("    "),
        text("Mode: ") | dim,
        text(modeName) | bold | color(Color::Cyan),
        text("  (m to toggle)") | dim,
    });
    return vbox(Elements{
        titleRow,
        text(""),
        makeMeterRow(meters, selectedIdx, data, registry, midiMap),
        text(""),
        hbox(Elements{
            text(" Envelope drives pre-gain reduction and post-gain compensation") | dim,
        }),
    });
}

} // namespace hexcaster::tui
