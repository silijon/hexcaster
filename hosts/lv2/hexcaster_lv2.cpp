/**
 * hexcaster_lv2.cpp -- LV2 plugin wrapper for HexCaster.
 *
 * Thin host layer over hexcaster_pipeline + hexcaster_params.
 *
 * Port layout:
 *   0 -- Audio In       (lv2:AudioPort,   lv2:InputPort)
 *   1 -- Audio Out      (lv2:AudioPort,   lv2:OutputPort)
 *   2 -- Input Gain dB  (lv2:ControlPort, lv2:InputPort)  [-60, +24], default 0
 *   3 -- Model Reload   (lv2:ControlPort, lv2:InputPort)  [0, 1], default 0
 *                       Toggle from 0 -> 1 to trigger model load.
 *
 * Model loading workflow:
 *   1. Write the full path to your .nam file into:
 *        ~/.config/hexcaster/model_path
 *      Example:
 *        echo "/home/john/models/my_amp.nam" > ~/.config/hexcaster/model_path
 *   2. In Reaper, toggle "Model Reload" to 1. The plugin fires a background
 *      thread to load the model without blocking the audio thread.
 *   3. The model is live within ~1 second (depending on model size).
 *
 * The loaded model path is persisted via LV2 state (state:interface), so
 * Reaper will reload the model automatically when the project is reopened.
 */

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/nam_stage.h"
#include "hexcaster/noise_gate.h"
#include "hexcaster/eq.h"
#include "hexcaster/bloom_controller.h"
#include "hexcaster/param_registry.h"

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <thread>

#define HEXCASTER_URI       "urn:hexcaster:hexcaster"
#define HEXCASTER_MODEL_URI "urn:hexcaster:model_path"

namespace {

// Path to the sidecar file where the model path is written externally.
// Reads $HOME at runtime so this works for any user.
static std::string sidecarPath()
{
    const char* home = std::getenv("HOME");
    if (!home) home = "/home/john";
    return std::string(home) + "/.config/hexcaster/model_path";
}

static std::string readSidecar()
{
    std::ifstream f(sidecarPath());
    if (!f.is_open()) return {};
    std::string path;
    std::getline(f, path);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' ||
                              path.back() == ' '))
        path.pop_back();
    return path;
}

// ---------------------------------------------------------------------------
// Port indices
// ---------------------------------------------------------------------------

enum PortIndex {
    PORT_AUDIO_IN     = 0,
    PORT_AUDIO_OUT    = 1,
    PORT_INPUT_GAIN   = 2,
    PORT_MODEL_RELOAD = 3,
    PORT_COUNT
};

// ---------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------

struct HexCasterLV2 {
    // Ports (set by connect_port, valid during run())
    const float* audioIn        = nullptr;
    float*       audioOut       = nullptr;
    const float* inputGainCtl   = nullptr;
    const float* modelReloadCtl = nullptr;

    // Edge-detect for reload trigger
    float prevReloadValue = 0.f;

    // Background loader state
    // The audio thread only sets loadRequested_ and reads loadComplete_.
    // The loader thread reads loadRequested_, does the work, then sets loadComplete_.
    std::atomic<bool> loadRequested_{ false };
    std::atomic<bool> loadComplete_{ false };
    std::string       pendingLoadPath_;   // written before loadRequested_, read by loader thread
    std::thread       loaderThread_;

    // URID map (for state interface)
    LV2_URID_Map* uridMap      = nullptr;
    LV2_URID      uridAtomPath = 0;
    LV2_URID      uridModelUri = 0;

    // DSP
    hexcaster::ParamRegistry   params;
    hexcaster::NoiseGate       noiseGate;
    hexcaster::GainStage       inputGain;
    hexcaster::GainStage       bloomPreGain;
    hexcaster::NamStage        nam;
    hexcaster::GainStage       bloomPostGain;
    hexcaster::MidSweepEQ      eq;
    hexcaster::GainStage       masterVolume;
    hexcaster::BloomController bloom;
    hexcaster::Pipeline        pipeline;

    explicit HexCasterLV2(double sampleRate, const LV2_Feature* const* features)
        : bloom(bloomPreGain, bloomPostGain)
    {
        for (int i = 0; features && features[i]; ++i) {
            if (std::strcmp(features[i]->URI, LV2_URID__map) == 0) {
                uridMap = static_cast<LV2_URID_Map*>(features[i]->data);
            }
        }

        if (uridMap) {
            uridAtomPath = uridMap->map(uridMap->handle, LV2_ATOM__Path);
            uridModelUri = uridMap->map(uridMap->handle, HEXCASTER_MODEL_URI);
        }

        noiseGate.setThresholdDb(params.get(hexcaster::ParamId::NoiseGateThreshold_dB));
        inputGain.setGainDb(params.get(hexcaster::ParamId::InputGain_dB));

        pipeline.addStage(&noiseGate);      // stage 0
        pipeline.addStage(&inputGain);      // stage 1
        pipeline.addStage(&bloomPreGain);   // stage 2
        pipeline.addStage(&nam);            // stage 3
        pipeline.addStage(&bloomPostGain);  // stage 4
        pipeline.addStage(&eq);             // stage 5
        pipeline.addStage(&masterVolume);   // stage 6
        pipeline.addController(&bloom);
        pipeline.prepare(static_cast<float>(sampleRate), 4096);
        bloom.prepare(static_cast<float>(sampleRate), 4096);
    }

    ~HexCasterLV2()
    {
        // Ensure the loader thread is finished before destruction.
        if (loaderThread_.joinable())
            loaderThread_.join();
    }

    // Spawn a background thread to load the model. Safe to call from run().
    void triggerLoad(const std::string& path)
    {
        // If a previous load is still in flight, let it finish first.
        if (loaderThread_.joinable())
            loaderThread_.join();

        pendingLoadPath_ = path;
        loadComplete_.store(false, std::memory_order_release);
        loadRequested_.store(true, std::memory_order_release);

        loaderThread_ = std::thread([this]() {
            const std::string p = pendingLoadPath_;
            nam.loadModel(p);
            loadComplete_.store(true, std::memory_order_release);
        });
    }
};

// ---------------------------------------------------------------------------
// LV2 core callbacks
// ---------------------------------------------------------------------------

static LV2_Handle instantiate(const LV2_Descriptor*    /*descriptor*/,
                               double                    sampleRate,
                               const char*               /*bundlePath*/,
                               const LV2_Feature* const* features)
{
    void* mem = std::malloc(sizeof(HexCasterLV2));
    if (!mem) return nullptr;
    return new (mem) HexCasterLV2(sampleRate, features);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    switch (static_cast<PortIndex>(port)) {
        case PORT_AUDIO_IN:
            self->audioIn        = static_cast<const float*>(data); break;
        case PORT_AUDIO_OUT:
            self->audioOut       = static_cast<float*>(data);       break;
        case PORT_INPUT_GAIN:
            self->inputGainCtl   = static_cast<const float*>(data); break;
        case PORT_MODEL_RELOAD:
            self->modelReloadCtl = static_cast<const float*>(data); break;
        default: break;
    }
}

static void activate(LV2_Handle /*instance*/) {}

static void run(LV2_Handle instance, uint32_t sampleCount)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    if (!self->audioIn || !self->audioOut) return;

    // Sync params -> stages each block. Reads are atomic; no locks.
    self->noiseGate.setThresholdDb(
        self->params.get(hexcaster::ParamId::NoiseGateThreshold_dB));

    if (self->inputGainCtl) {
        self->inputGain.setGainDb(*self->inputGainCtl);
    }

    self->bloom.setBasePreDb   (self->params.get(hexcaster::ParamId::BloomBasePre_dB));
    self->bloom.setBasePostDb  (self->params.get(hexcaster::ParamId::BloomBasePost_dB));
    self->bloom.setDepth       (self->params.get(hexcaster::ParamId::BloomDepth));
    self->bloom.setCompensation(self->params.get(hexcaster::ParamId::BloomCompensation));
    self->bloom.setAttackMs    (self->params.get(hexcaster::ParamId::EnvAttackMs));
    self->bloom.setReleaseMs   (self->params.get(hexcaster::ParamId::EnvReleaseMs));
    self->eq.setGainDb         (self->params.get(hexcaster::ParamId::EqGain_dB));
    self->eq.setSweepHz        (self->params.get(hexcaster::ParamId::EqSweepHz));
    self->eq.setQ              (self->params.get(hexcaster::ParamId::EqQ));
    self->masterVolume.setGainDb(self->params.get(hexcaster::ParamId::MasterVolume_dB));

    // Model reload trigger: fire background load on 0 -> 1 rising edge only.
    if (self->modelReloadCtl) {
        const float cur = *self->modelReloadCtl;
        if (cur >= 0.5f && self->prevReloadValue < 0.5f) {
            const std::string path = readSidecar();
            if (!path.empty()) {
                self->triggerLoad(path);
            }
        }
        self->prevReloadValue = cur;
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

// ---------------------------------------------------------------------------
// LV2 State -- persists the loaded model path across sessions
// ---------------------------------------------------------------------------

static LV2_State_Status state_save(LV2_Handle                instance,
                                    LV2_State_Store_Function  store,
                                    LV2_State_Handle          handle,
                                    uint32_t                  /*flags*/,
                                    const LV2_Feature* const* /*features*/)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    if (!self->uridMap) return LV2_STATE_ERR_NO_FEATURE;

    const std::string& path = self->nam.modelPath();
    if (path.empty()) return LV2_STATE_SUCCESS;

    store(handle,
          self->uridModelUri,
          path.c_str(),
          path.size() + 1,
          self->uridAtomPath,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

    return LV2_STATE_SUCCESS;
}

static LV2_State_Status state_restore(LV2_Handle                  instance,
                                       LV2_State_Retrieve_Function retrieve,
                                       LV2_State_Handle            handle,
                                       uint32_t                    /*flags*/,
                                       const LV2_Feature* const*   /*features*/)
{
    auto* self = static_cast<HexCasterLV2*>(instance);
    if (!self->uridMap) return LV2_STATE_ERR_NO_FEATURE;

    uint32_t    type   = 0;
    uint32_t    rflags = 0;
    std::size_t size   = 0;

    const void* data = retrieve(handle, self->uridModelUri, &size, &type, &rflags);
    if (!data || size == 0) return LV2_STATE_SUCCESS;

    const std::string path(static_cast<const char*>(data));
    if (!path.empty()) {
        // state_restore is called outside the audio thread -- load directly.
        self->nam.loadModel(path);
    }

    return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface kStateInterface = { state_save, state_restore };

// ---------------------------------------------------------------------------
// Extension data
// ---------------------------------------------------------------------------

static const void* extension_data(const char* uri)
{
    if (std::strcmp(uri, LV2_STATE__interface) == 0) return &kStateInterface;
    return nullptr;
}

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
