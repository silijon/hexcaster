/**
 * hexcaster_lv2.cpp -- LV2 plugin wrapper for HexCaster.
 *
 * Thin host layer over hexcaster_pipeline + hexcaster_params.
 *
 * Port layout:
 *   0 -- Audio In       (lv2:AudioPort,   lv2:InputPort)
 *   1 -- Audio Out      (lv2:AudioPort,   lv2:OutputPort)
 *   2 -- Master Gain dB (lv2:ControlPort, lv2:InputPort)  [-60, +24], default 0
 */

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/param_registry.h"

#include <lv2/core/lv2.h>
#include <cstdlib>
#include <cstring>
#include <new>

#define HEXCASTER_URI "urn:hexcaster:hexcaster"

namespace {

enum PortIndex {
    PORT_AUDIO_IN    = 0,
    PORT_AUDIO_OUT   = 1,
    PORT_MASTER_GAIN = 2,
    PORT_COUNT
};

struct HexCasterLV2 {
    // Ports (set by connect_port, valid during run())
    const float* audioIn       = nullptr;
    float*       audioOut      = nullptr;
    const float* masterGainCtl = nullptr;

    // DSP
    hexcaster::ParamRegistry params;
    hexcaster::GainStage     masterGain;
    hexcaster::Pipeline      pipeline;

    explicit HexCasterLV2(double sampleRate)
    {
        masterGain.setGainDb(params.get(hexcaster::ParamId::MasterGain_dB));
        pipeline.addStage(&masterGain);
        pipeline.prepare(static_cast<float>(sampleRate), 4096);
    }
};

// ---------------------------------------------------------------------------
// LV2 callbacks
// ---------------------------------------------------------------------------

static LV2_Handle instantiate(const LV2_Descriptor* /*descriptor*/,
                               double                 sampleRate,
                               const char*            /*bundlePath*/,
                               const LV2_Feature* const* /*features*/)
{
    // Use placement/operator new -- no exceptions, return null on failure.
    void* mem = std::malloc(sizeof(HexCasterLV2));
    if (!mem) return nullptr;

    return new (mem) HexCasterLV2(sampleRate);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    switch (static_cast<PortIndex>(port)) {
        case PORT_AUDIO_IN:    self->audioIn       = static_cast<const float*>(data); break;
        case PORT_AUDIO_OUT:   self->audioOut      = static_cast<float*>(data);       break;
        case PORT_MASTER_GAIN: self->masterGainCtl = static_cast<const float*>(data); break;
        default: break;
    }
}

static void activate(LV2_Handle /*instance*/) {}

static void run(LV2_Handle instance, uint32_t sampleCount)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    if (!self->audioIn || !self->audioOut) return;

    // Read master gain control port and apply to gain stage each block.
    if (self->masterGainCtl) {
        self->masterGain.setGainDb(*self->masterGainCtl);
    }

    // Copy input -> output, then process in-place.
    std::memcpy(self->audioOut, self->audioIn, sampleCount * sizeof(float));
    self->pipeline.process(self->audioOut, static_cast<int>(sampleCount));
}

static void deactivate(LV2_Handle /*instance*/) {}

static void cleanup(LV2_Handle instance)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    self->~HexCasterLV2();
    std::free(self);
}

static const void* extension_data(const char* /*uri*/) { return nullptr; }

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const LV2_Descriptor kDescriptor = {
    HEXCASTER_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

} // namespace

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &kDescriptor : nullptr;
}
