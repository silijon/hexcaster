/**
 * hexcaster_clap.cpp -- CLAP plugin wrapper for HexCaster.
 *
 * Thin host layer over hexcaster_pipeline + hexcaster_params.
 * Mirrors the LV2 wrapper design: same DSP objects, same parameter set,
 * same background model-loading pattern.
 *
 * Audio layout: mono in, mono out (single channel).
 *
 * Parameters: all 17 ParamId entries + 1 Model Reload trigger + 5 read-only meters (23 total).
 *   - ParamId enum values are used directly as CLAP param_ids.
 *   - Model Reload param_id = 100 (stepped [0,1], edge-detect 0->1 in process).
 *   - Meter param_ids 200-204: read-only, pushed to out_events each block so
 *     Reaper's default plugin UI shows live-updating sliders.
 *
 * Model loading workflow:
 *   1. Write the full path to your .nam file into:
 *        ~/.config/hexcaster/model_path
 *   2. In Reaper, set "Model Reload" to 1. The plugin fires a background
 *      thread to load the model without blocking the audio thread.
 *   3. The model is live within ~1 second (depending on model size).
 *
 * The loaded model path is persisted via CLAP state (clap_plugin_state_t),
 * so Reaper reloads the model automatically when the project is reopened.
 */

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/nam_stage.h"
#include "hexcaster/noise_gate.h"
#include "hexcaster/eq.h"
#include "hexcaster/bloom_controller.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/param_id.h"

#include <clap/clap.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <new>  // IWYU pragma: keep  (placement new)
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr clap_id kModelReloadParamId  = 100;

// Read-only meter param ids (CLAP-host-only, not in ParamRegistry)
static constexpr clap_id kMeterGainEnv       = 200;
static constexpr clap_id kMeterDetectorEnv   = 201;
static constexpr clap_id kMeterDetectorPeak  = 202;
static constexpr clap_id kMeterActivity      = 203;
static constexpr clap_id kMeterPreDb         = 204;
static constexpr clap_id kMeterPostDb        = 205;

// ---------------------------------------------------------------------------
// Sidecar helpers (same as LV2 wrapper)
// ---------------------------------------------------------------------------

namespace {

static std::string sidecarPath()
{
    const char* home = std::getenv("HOME");
    if (!home) home = "";
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
// Parameter table
// ---------------------------------------------------------------------------

// Registry-backed params (id < kModelReloadParamId) carry no inline numeric
// ranges — min/max/default are read from ParamRegistry at runtime so there
// is a single source of truth. Non-registry params (Model Reload, meters)
// carry their own inline ranges via the extra fields below.
struct ClapParamMeta {
    clap_id     id;
    const char* name;
    const char* module;   // "/" separated path for grouping in host
    bool        isStepped;
    bool        isReadOnly;
    // Used only when id >= kModelReloadParamId (non-registry params):
    double      minVal;
    double      maxVal;
    double      defaultVal;
};

static constexpr ClapParamMeta kParams[] = {
    // Registry-backed DSP params: numeric ranges come from ParamRegistry.
    // id                                                    name                      module        stepped  readOnly  min    max    default
    { (clap_id)hexcaster::ParamId::NoiseGateThreshold_dB, "Gate Threshold",    "Noise Gate", false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::NoiseGateAttackMs,     "Gate Attack",       "Noise Gate", false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::NoiseGateReleaseMs,    "Gate Release",      "Noise Gate", false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::NoiseGateHoldMs,       "Gate Hold",         "Noise Gate", false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::InputGain_dB,          "Input Gain",        "Input",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomBasePre_dB,       "Bloom Pre Gain",    "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomBasePost_dB,      "Bloom Post Gain",   "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomDepth_dB,         "Bloom Depth",       "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomCompensation,     "Bloom Compensation","Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomAttackMs,         "Bloom Attack",      "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomReleaseMs,        "Bloom Release",     "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomSensitivity_dB,   "Bloom Sensitivity", "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::BloomActivityThreshold,"Bloom Activity",    "Bloom",      false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::EqGain_dB,             "EQ Gain",           "EQ",         false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::EqSweepHz,             "EQ Sweep",          "EQ",         false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::EqQ,                   "EQ Q",              "EQ",         false, false,  0.0,  0.0, 0.0 },
    { (clap_id)hexcaster::ParamId::MasterVolume_dB,       "Master Volume",     "Output",     false, false,  0.0,  0.0, 0.0 },
    // Non-registry params: inline ranges used directly.
    { kModelReloadParamId, "Model Reload",      "Model",   true,  false, 0.0,  1.0, 0.0 },
    // Read-only meters: updated via out_events each process() block.
    { kMeterGainEnv,      "Gain Envelope",     "Meters",  false, true,  0.0,   1.0, 0.0 },
    { kMeterDetectorEnv,  "Detector Envelope", "Meters",  false, true,  0.0,   1.0, 0.0 },
    { kMeterDetectorPeak, "Detector Peak", "Meters",  false, true,  0.0,   1.0, 0.0 },
    { kMeterActivity,     "Harmonic Activity", "Meters",  false, true,  0.0,   1.0, 0.0 },
    { kMeterPreDb,        "Pre Gain Applied",  "Meters",  false, true, -60.0, 32.0, 0.0 },
    { kMeterPostDb,       "Post Gain Applied", "Meters",  false, true, -60.0, 32.0, 0.0 },
};
static constexpr uint32_t kNumParams = sizeof(kParams) / sizeof(kParams[0]);

static const ClapParamMeta* findParam(clap_id id)
{
    for (uint32_t i = 0; i < kNumParams; ++i) {
        if (kParams[i].id == id) return &kParams[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------

struct HexCasterCLAP {
    clap_plugin_t          plugin;   // Must be first member
    const clap_host_t*     host;

    // DSP (same layout as LV2 wrapper)
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

    // Background model loader
    std::atomic<bool> loadRequested_{ false };
    std::atomic<bool> loadComplete_{ false };
    std::string       pendingLoadPath_;
    std::thread       loaderThread_;

    // Model reload trigger edge detection
    float prevReloadValue = 0.f;

    // Cached meter values — compared each block to avoid redundant out_events
    float prevMeterGainEnv_  = 0.f;
    float prevMeterDetEnv_   = 0.f;
    float prevMeterDetPeak_  = 0.f;
    float prevMeterActivity_ = 0.f;
    float prevMeterPreDb_    = 0.f;
    float prevMeterPostDb_   = 0.f;

    explicit HexCasterCLAP(const clap_host_t* h)
        : host(h)
        , bloom(bloomPreGain, bloomPostGain)
    {

        pipeline.addStage(&noiseGate);
        pipeline.addStage(&inputGain);
        pipeline.addStage(&bloomPreGain);
        pipeline.addStage(&nam);
        pipeline.addStage(&bloomPostGain);
        pipeline.addStage(&eq);
        pipeline.addStage(&masterVolume);
        pipeline.addController(&bloom);
    }

    ~HexCasterCLAP()
    {
        if (loaderThread_.joinable())
            loaderThread_.join();
    }

    void triggerLoad(const std::string& path)
    {
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

    // Push all param registry values into DSP stages.
    // Called at the top of every process() block.
    void syncParams()
    {
        using P = hexcaster::ParamId;

        noiseGate.setThresholdDb(params.get(P::NoiseGateThreshold_dB));

        inputGain.setGainDb(params.get(P::InputGain_dB));

        bloom.setBasePreDb        (params.get(P::BloomBasePre_dB));
        bloom.setBasePostDb       (params.get(P::BloomBasePost_dB));
        bloom.setDepth            (params.get(P::BloomDepth_dB));
        bloom.setCompensation     (params.get(P::BloomCompensation));
        bloom.setAttackMs         (params.get(P::BloomAttackMs));
        bloom.setReleaseMs        (params.get(P::BloomReleaseMs));
        bloom.setSensitivity      (params.get(P::BloomSensitivity_dB));
        bloom.setActivityThreshold(params.get(P::BloomActivityThreshold));

        eq.setGainDb (params.get(P::EqGain_dB));
        eq.setSweepHz(params.get(P::EqSweepHz));
        eq.setQ      (params.get(P::EqQ));

        masterVolume.setGainDb(params.get(P::MasterVolume_dB));
    }
};

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

static bool plugin_init(const clap_plugin_t* plugin)
{
    // Pipeline and DSP were already set up in the constructor.
    // activate() is where prepare() is called with the actual sample rate.
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t* plugin)
{
    auto* self = static_cast<HexCasterCLAP*>(plugin->plugin_data);
    self->~HexCasterCLAP();
    std::free(self);
}

static bool plugin_activate(const clap_plugin_t* plugin,
                             double               sampleRate,
                             uint32_t             /*minFramesCount*/,
                             uint32_t             maxFramesCount)
{
    auto* self = static_cast<HexCasterCLAP*>(plugin->plugin_data);
    const auto sr    = static_cast<float>(sampleRate);
    const auto nmax  = static_cast<int>(maxFramesCount);
    self->pipeline.prepare(sr, nmax);
    self->bloom.prepare(sr, nmax);
    return true;
}

static void plugin_deactivate(const clap_plugin_t* /*plugin*/) {}

static bool plugin_start_processing(const clap_plugin_t* /*plugin*/) { return true; }
static void plugin_stop_processing (const clap_plugin_t* /*plugin*/) {}
static void plugin_reset           (const clap_plugin_t* /*plugin*/) {}

static clap_process_status plugin_process(const clap_plugin_t*  plugin,
                                           const clap_process_t* proc)
{
    auto* self = static_cast<HexCasterCLAP*>(plugin->plugin_data);

    // Process incoming parameter events
    const clap_input_events_t* inEv = proc->in_events;
    const uint32_t numEv = inEv->size(inEv);
    for (uint32_t i = 0; i < numEv; ++i) {
        const clap_event_header_t* hdr = inEv->get(inEv, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            if (ev->param_id == kModelReloadParamId) {
                // Handled below via edge detection on prevReloadValue
                const float cur = static_cast<float>(ev->value);
                if (cur >= 0.5f && self->prevReloadValue < 0.5f) {
                    const std::string path = readSidecar();
                    if (!path.empty()) self->triggerLoad(path);
                }
                self->prevReloadValue = cur;
            } else {
                self->params.set(
                    static_cast<hexcaster::ParamId>(ev->param_id),
                    static_cast<float>(ev->value));
            }
        }
    }

    self->syncParams();

    // Copy input -> output, then process in-place (mono)
    if (proc->audio_inputs_count == 0 || proc->audio_outputs_count == 0) {
        return CLAP_PROCESS_CONTINUE;
    }
    const float* in  = proc->audio_inputs[0].data32[0];
    float*       out = proc->audio_outputs[0].data32[0];
    const uint32_t nframes = proc->frames_count;

    std::memcpy(out, in, nframes * sizeof(float));
    self->pipeline.process(out, static_cast<int>(nframes));

    // Push read-only meter params to out_events so Reaper's UI sliders update.
    // Only send events when the value has changed meaningfully.
    auto pushMeter = [&](clap_id id, float& prev, float cur) {
        if (std::fabs(cur - prev) < 0.001f) return;
        prev = cur;
        clap_event_param_value_t ev{};
        ev.header.size     = sizeof(ev);
        ev.header.time     = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags    = 0;
        ev.param_id        = id;
        ev.cookie          = nullptr;
        ev.value           = static_cast<double>(cur);
        proc->out_events->try_push(proc->out_events, &ev.header);
    };

    pushMeter(kMeterGainEnv,      self->prevMeterGainEnv_,  self->bloom.getGainEnvelope());
    pushMeter(kMeterDetectorEnv,  self->prevMeterDetEnv_,   self->bloom.getDetectorEnvelope());
    pushMeter(kMeterDetectorPeak, self->prevMeterDetPeak_,   self->bloom.getDetectorPeak());
    pushMeter(kMeterActivity,     self->prevMeterActivity_, self->bloom.getHarmonicActivity());
    pushMeter(kMeterPreDb,        self->prevMeterPreDb_,    self->bloomPreGain.getGainDb());
    pushMeter(kMeterPostDb,       self->prevMeterPostDb_,   self->bloomPostGain.getGainDb());

    return CLAP_PROCESS_CONTINUE;
}

static const void* plugin_get_extension(const clap_plugin_t* plugin,
                                         const char*           id);

static void plugin_on_main_thread(const clap_plugin_t* /*plugin*/) {}

// ---------------------------------------------------------------------------
// Extension: audio ports
// ---------------------------------------------------------------------------

static uint32_t audio_ports_count(const clap_plugin_t* /*plugin*/, bool /*isInput*/)
{
    return 1;
}

static bool audio_ports_get(const clap_plugin_t* /*plugin*/,
                              uint32_t             index,
                              bool                 /*isInput*/,
                              clap_audio_port_info_t* info)
{
    if (index != 0) return false;
    info->id            = 0;
    info->channel_count = 1;
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type     = CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    std::strncpy(info->name, "Audio", sizeof(info->name));
    return true;
}

static const clap_plugin_audio_ports_t kAudioPorts = {
    audio_ports_count,
    audio_ports_get,
};

// ---------------------------------------------------------------------------
// Extension: parameters
// ---------------------------------------------------------------------------

static uint32_t params_count(const clap_plugin_t* /*plugin*/)
{
    return kNumParams;
}

static bool params_get_info(const clap_plugin_t* plugin,
                              uint32_t               paramIndex,
                              clap_param_info_t*     info)
{
    if (paramIndex >= kNumParams) return false;
    const ClapParamMeta& m = kParams[paramIndex];

    info->id     = m.id;
    info->flags  = m.isReadOnly ? CLAP_PARAM_IS_READONLY
                                : CLAP_PARAM_IS_AUTOMATABLE;
    if (m.isStepped) info->flags |= CLAP_PARAM_IS_STEPPED;
    info->cookie = nullptr;
    std::strncpy(info->name,   m.name,   sizeof(info->name));
    std::strncpy(info->module, m.module, sizeof(info->module));

    if (m.id < kModelReloadParamId) {
        // Registry-backed: read ranges from ParamRegistry (single source of truth).
        const auto* self = static_cast<const HexCasterCLAP*>(plugin->plugin_data);
        const auto  pid  = static_cast<hexcaster::ParamId>(m.id);
        const auto  rng  = self->params.getRange(pid);
        info->min_value     = static_cast<double>(rng.min);
        info->max_value     = static_cast<double>(rng.max);
        info->default_value = static_cast<double>(
            hexcaster::ParamRegistry::getDefault(pid));
    } else {
        // Non-registry (Model Reload, meters): use inline values.
        info->min_value     = m.minVal;
        info->max_value     = m.maxVal;
        info->default_value = m.defaultVal;
    }
    return true;
}

static bool params_get_value(const clap_plugin_t* plugin,
                               clap_id              paramId,
                               double*              value)
{
    const auto* self = static_cast<const HexCasterCLAP*>(plugin->plugin_data);
    if (paramId == kModelReloadParamId) { *value = self->prevReloadValue;      return true; }
    if (paramId == kMeterGainEnv)       { *value = self->prevMeterGainEnv_;    return true; }
    if (paramId == kMeterDetectorEnv)   { *value = self->prevMeterDetEnv_;     return true; }
    if (paramId == kMeterDetectorPeak)  { *value = self->prevMeterDetPeak_;     return true; }
    if (paramId == kMeterActivity)      { *value = self->prevMeterActivity_;   return true; }
    if (paramId == kMeterPreDb)         { *value = self->prevMeterPreDb_;      return true; }
    if (paramId == kMeterPostDb)        { *value = self->prevMeterPostDb_;     return true; }

    const ClapParamMeta* m = findParam(paramId);
    if (!m) return false;
    *value = static_cast<double>(
        self->params.get(static_cast<hexcaster::ParamId>(paramId)));
    return true;
}

static bool params_value_to_text(const clap_plugin_t* /*plugin*/,
                                   clap_id              paramId,
                                   double               value,
                                   char*                buf,
                                   uint32_t             bufSize)
{
    const ClapParamMeta* m = findParam(paramId);
    if (!m) return false;

    // Choose a unit suffix based on the parameter name
    const char* unit = "";
    const char* name = m->name;
    if (std::strstr(name, "Gain") || std::strstr(name, "Threshold") ||
        std::strstr(name, "Volume") || std::strstr(name, "Depth") ||
        std::strstr(name, "Sensitivity")) {
        unit = " dB";
    } else if (std::strstr(name, "Attack") || std::strstr(name, "Release") ||
               std::strstr(name, "Hold")) {
        unit = " ms";
    } else if (std::strstr(name, "Sweep")) {
        unit = " Hz";
    }

    std::snprintf(buf, bufSize, "%.2f%s", value, unit);
    return true;
}

static bool params_text_to_value(const clap_plugin_t* /*plugin*/,
                                   clap_id              paramId,
                                   const char*          text,
                                   double*              value)
{
    const ClapParamMeta* m = findParam(paramId);
    if (!m) return false;
    char* end = nullptr;
    *value = std::strtod(text, &end);
    return end != text;
}

static void params_flush(const clap_plugin_t*        plugin,
                           const clap_input_events_t*  inEv,
                           const clap_output_events_t* /*outEv*/)
{
    auto* self = static_cast<HexCasterCLAP*>(plugin->plugin_data);
    const uint32_t numEv = inEv->size(inEv);
    for (uint32_t i = 0; i < numEv; ++i) {
        const clap_event_header_t* hdr = inEv->get(inEv, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            if (ev->param_id != kModelReloadParamId) {
                self->params.set(
                    static_cast<hexcaster::ParamId>(ev->param_id),
                    static_cast<float>(ev->value));
            }
        }
    }
}

static const clap_plugin_params_t kParams_ext = {
    params_count,
    params_get_info,
    params_get_value,
    params_value_to_text,
    params_text_to_value,
    params_flush,
};

// ---------------------------------------------------------------------------
// Extension: state (persist model path in Reaper project)
// ---------------------------------------------------------------------------

static bool state_save(const clap_plugin_t*   plugin,
                        const clap_ostream_t*  stream)
{
    const auto* self = static_cast<const HexCasterCLAP*>(plugin->plugin_data);
    const std::string& path = self->nam.modelPath();
    // Write length prefix (uint32) then the path bytes (no null terminator)
    const uint32_t len = static_cast<uint32_t>(path.size());
    if (stream->write(stream, &len, sizeof(len)) != sizeof(len)) return false;
    if (len > 0) {
        if (stream->write(stream, path.c_str(), len) != static_cast<int64_t>(len))
            return false;
    }
    return true;
}

static bool state_load(const clap_plugin_t*   plugin,
                        const clap_istream_t*  stream)
{
    auto* self = static_cast<HexCasterCLAP*>(plugin->plugin_data);
    uint32_t len = 0;
    if (stream->read(stream, &len, sizeof(len)) != sizeof(len)) return false;
    if (len == 0) return true;

    std::string path(len, '\0');
    if (stream->read(stream, path.data(), len) != static_cast<int64_t>(len))
        return false;

    // state_load is called outside the audio thread -- safe to load directly
    self->nam.loadModel(path);
    return true;
}

static const clap_plugin_state_t kState = {
    state_save,
    state_load,
};

// ---------------------------------------------------------------------------
// Extension dispatch
// ---------------------------------------------------------------------------

static const void* plugin_get_extension(const clap_plugin_t* /*plugin*/,
                                          const char*           id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS)      == 0) return &kParams_ext;
    if (std::strcmp(id, CLAP_EXT_STATE)        == 0) return &kState;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const char* kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_DISTORTION,
    nullptr,
};

static const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION,
    "com.hexcaster.hexcaster",
    "HexCaster",
    "HexCaster",
    "",   // url
    "",   // manual_url
    "",   // support_url
    "0.1.0",
    "Neural amp modeler with Bloom dynamics",
    kFeatures,
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t* /*factory*/)
{
    return 1;
}

static const clap_plugin_descriptor_t*
factory_get_plugin_descriptor(const clap_plugin_factory_t* /*factory*/, uint32_t index)
{
    return (index == 0) ? &kDescriptor : nullptr;
}

static const clap_plugin_t*
factory_create_plugin(const clap_plugin_factory_t* /*factory*/,
                      const clap_host_t*            host,
                      const char*                   pluginId)
{
    if (std::strcmp(pluginId, kDescriptor.id) != 0) return nullptr;

    void* mem = std::malloc(sizeof(HexCasterCLAP));
    if (!mem) return nullptr;

    auto* inst = new (mem) HexCasterCLAP(host);

    inst->plugin.desc        = &kDescriptor;
    inst->plugin.plugin_data = inst;
    inst->plugin.init             = plugin_init;
    inst->plugin.destroy          = plugin_destroy;
    inst->plugin.activate         = plugin_activate;
    inst->plugin.deactivate       = plugin_deactivate;
    inst->plugin.start_processing = plugin_start_processing;
    inst->plugin.stop_processing  = plugin_stop_processing;
    inst->plugin.reset            = plugin_reset;
    inst->plugin.process          = plugin_process;
    inst->plugin.get_extension    = plugin_get_extension;
    inst->plugin.on_main_thread   = plugin_on_main_thread;

    return &inst->plugin;
}

static const clap_plugin_factory_t kFactory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    factory_create_plugin,
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static bool entry_init(const char* /*pluginPath*/)       { return true; }
static void entry_deinit()                                {}
static const void* entry_get_factory(const char* factoryId)
{
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" {

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    entry_init,
    entry_deinit,
    entry_get_factory,
};

} // extern "C"
