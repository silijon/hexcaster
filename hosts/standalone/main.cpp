#include "audio_engine.h"
#include "alsa_audio_engine.h"
#include "midi_input.h"

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/nam_stage.h"
#include "hexcaster/noise_gate.h"
#include "hexcaster/eq.h"
#include "hexcaster/bloom_controller.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"
#include "hexcaster/param_id.h"
#include "hexcaster/input_gain.h"

    bool runtimeFailed = false;

#ifdef HEXCASTER_TUI_ENABLED
#include "tui/tui.h"
#endif

#include "level_meter.h"

#include <atomic>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <alsa/asoundlib.h>

// ---------------------------------------------------------------------------
// Signal handling -- Ctrl+C sets this flag, watcher thread calls engine.stop()
// ---------------------------------------------------------------------------

static std::atomic<bool> gQuit{ false };

static void handleSignal(int /*sig*/)
{
    gQuit.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

struct MidiCcMapping {
    uint8_t           cc;
    hexcaster::ParamId paramId;
};

struct Args {
    std::string  inputDevice    = "hw:2,0";
    std::string  outputDevice   = "hw:2,0";
    std::string  modelPath;
    hexcaster::NamQualityPolicy namQuality = hexcaster::NamQualityPolicy::Auto;
    std::string  midiDevice;                    // empty = MIDI disabled
    unsigned int sampleRate     = 48000;
    unsigned int bufferFrames   = 128;
    float        gainDb                = 0.f;
    float        inputLevelDBu         = hexcaster::kDefaultInterfaceInputLevelDBu;
    float        gateThresholdDb      = -60.f;
    float        eqHighShelfGainDb    = 0.f;
    float        eqLowShelfGainDb     = 0.f;
    float        masterVolumeDb       = 0.f;
    float        bloomDepth           = 6.f;
    float        bloomCompensation    = 0.5f;
    int          inputChannel         = 0;
    bool         listDevices    = false;
    bool         listMidi       = false;
    bool         help           = false;
    bool         tui            = false;
    bool         realtimeMetrics = false;
    std::vector<MidiCcMapping> midiMappings;
};

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s --model <path.nam> [options]\n"
        "\n"
        "Options:\n"
        "  --model <path>              NAM model file (.nam)  [required]\n"
        "  --nam-quality <mode>        A2 packed-model policy: auto, lite, full  [auto]\n"
        "  --device <hw:X,Y>           Set both input and output device\n"
        "  --input-device <dev>        Input audio device\n"
        "  --output-device <dev>       Output audio device\n"
        "  --sample-rate <Hz>          Sample rate  [default: 48000]\n"
        "  --buffer <frames>           Buffer size in frames  [default: 128]\n"
        "  --gain <dB>                 Initial model-relative input trim  [-12, +12] dB  [0]\n"
        "  --input-level-dbu <dBu>     Capture-interface level at 0 dBFS  [default: 11.63]\n"
        "  --gate-threshold <dB>       Noise gate threshold  [-80, 0] dB  [default: -60]\n"
        "  --high-shelf-gain <dB>      High-shelf EQ gain  [-32, +12] dB  [default: 0]\n"
        "  --low-shelf-gain <dB>       Low-shelf EQ gain  [-32, +12] dB  [default: 0]\n"
        "  --master-volume <dB>        Final output level to power amp  [-60, +24] dB  [default: 0]\n"
        "  --bloom-depth <dB>          Bloom max input gain reduction  [0, 24] dB  [default: 6]\n"
        "  --bloom-compensation <r>    Bloom output compensation ratio  [0, 2]  [default: 0.5]\n"
        "  --input-channel <N>         Capture channel: 0=left, 1=right  [default: 0]\n"
        "  --midi-device <hw:X,Y,Z>    ALSA raw MIDI input device\n"
        "  --midi-cc <cc>:<ParamName>  Map a MIDI CC to a parameter  (repeatable)\n"
        "  --list-devices              Print ALSA PCM devices and exit\n"
        "  --list-midi                 Print ALSA raw MIDI devices and exit\n"
        "  --tui                       Start in terminal UI mode\n"
        "  --rt-metrics                Collect per-block timing/xrun metrics (benchmarking)\n"
        "  --help                      Show this help and exit\n"
        "\n"
        "Parameter names for --midi-cc:\n"
        "  InputGain_dB         BloomBasePre_dB    BloomBasePost_dB\n"
        "  BloomDepth_dB  BloomCompensation  BloomSensitivity_dB  BloomAttackMs  BloomReleaseMs\n"
        "  NoiseGateThreshold_dB  NoiseGateAttackMs  NoiseGateReleaseMs  NoiseGateHoldMs\n"
        "  HighShelfHz  HighShelfGain_dB  HighShelfBw\n"
        "  LowShelfHz  LowShelfGain_dB  LowShelfBw  MasterVolume_dB\n"
        "\n"
        "Examples:\n"
        "  %s --model ~/amp.nam --input-device hw:CARD=i2,DEV=0 \\\n"
        "     --output-device hw:CARD=sndrpihifiberry,DEV=0\n"
        "\n"
        "  %s --model ~/amp.nam --input-device hw:CARD=i2,DEV=0 \\\n"
        "     --output-device hw:CARD=sndrpihifiberry,DEV=0 \\\n"
        "     --midi-device hw:1,0,0 \\\n"
        "     --midi-cc 7:InputGain_dB --midi-cc 1:BloomBasePre_dB\n",
        prog, prog, prog);
}

static bool parseMidiCc(const char* arg, MidiCcMapping& out)
{
    // Expected format: "<cc>:<ParamName>"  e.g. "7:InputGain_dB"
    const char* colon = std::strchr(arg, ':');
    if (!colon) {
        std::fprintf(stderr, "Error: --midi-cc requires format <cc>:<ParamName>, got '%s'\n", arg);
        return false;
    }

    const int cc = std::atoi(arg);
    if (cc < 0 || cc > 127) {
        std::fprintf(stderr, "Error: CC number must be 0-127, got %d\n", cc);
        return false;
    }

    hexcaster::ParamId id;
    if (!hexcaster::paramIdFromName(colon + 1, id)) {
        std::fprintf(stderr, "Error: unknown parameter name '%s'\n", colon + 1);
        return false;
    }

    out.cc      = static_cast<uint8_t>(cc);
    out.paramId = id;
    return true;
}

static bool parseArgs(int argc, char** argv, Args& args)
{
    for (int i = 1; i < argc; ++i) {
        const char* key = argv[i];
        auto nextArg = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "Error: %s requires an argument\n", key);
            return nullptr;
        };

        if (std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0) {
            args.help = true;
            return true;
        }
        if (std::strcmp(key, "--list-devices") == 0) {
            args.listDevices = true;
            return true;
        }
        if (std::strcmp(key, "--list-midi") == 0) {
            args.listMidi = true;
            return true;
        }
        if (std::strcmp(key, "--tui") == 0) {
            args.tui = true;
            continue;
        }
        if (std::strcmp(key, "--rt-metrics") == 0) {
            args.realtimeMetrics = true;
            continue;
        }

        if (std::strcmp(key, "--model") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.modelPath = v;
        } else if (std::strcmp(key, "--nam-quality") == 0) {
            const char* v = nextArg(); if (!v) return false;
            if (std::strcmp(v, "auto") == 0) args.namQuality = hexcaster::NamQualityPolicy::Auto;
            else if (std::strcmp(v, "lite") == 0) args.namQuality = hexcaster::NamQualityPolicy::Lite;
            else if (std::strcmp(v, "full") == 0) args.namQuality = hexcaster::NamQualityPolicy::Full;
            else {
                std::fprintf(stderr, "Error: --nam-quality must be auto, lite, or full\n");
                return false;
            }
        } else if (std::strcmp(key, "--device") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.inputDevice = args.outputDevice = v;
        } else if (std::strcmp(key, "--input-device") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.inputDevice = v;
        } else if (std::strcmp(key, "--output-device") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.outputDevice = v;
        } else if (std::strcmp(key, "--sample-rate") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.sampleRate = static_cast<unsigned int>(std::atoi(v));
        } else if (std::strcmp(key, "--buffer") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.bufferFrames = static_cast<unsigned int>(std::atoi(v));
        } else if (std::strcmp(key, "--gain") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.gainDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--input-level-dbu") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.inputLevelDBu = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--gate-threshold") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.gateThresholdDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--high-shelf-gain") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.eqHighShelfGainDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--low-shelf-gain") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.eqLowShelfGainDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--master-volume") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.masterVolumeDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--bloom-depth") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.bloomDepth = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--bloom-compensation") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.bloomCompensation = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--input-channel") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.inputChannel = std::atoi(v);
        } else if (std::strcmp(key, "--midi-device") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.midiDevice = v;
        } else if (std::strcmp(key, "--midi-cc") == 0) {
            const char* v = nextArg(); if (!v) return false;
            MidiCcMapping mapping;
            if (!parseMidiCc(v, mapping)) return false;
            args.midiMappings.push_back(mapping);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", key);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Device listing
// ---------------------------------------------------------------------------

static void listAlsaDevices()
{
    std::fprintf(stdout, "ALSA PCM devices:\n");
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        std::fprintf(stderr, "  (failed to enumerate devices)\n");
        return;
    }
    for (void** h = hints; *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");
        if (name && (ioid == nullptr ||
                     std::strcmp(ioid, "Input") == 0 ||
                     std::strcmp(ioid, "Output") == 0)) {
            std::fprintf(stdout, "  %-30s %s\n", name, desc ? desc : "");
        }
        free(name); free(desc); free(ioid);
    }
    snd_device_name_free_hint(hints);
}

static void listMidiDevices()
{
    std::fprintf(stdout, "ALSA raw MIDI devices:\n");
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "rawmidi", &hints) < 0) {
        std::fprintf(stderr, "  (failed to enumerate MIDI devices)\n");
        return;
    }
    for (void** h = hints; *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");
        if (name && (ioid == nullptr || std::strcmp(ioid, "Input") == 0)) {
            std::fprintf(stdout, "  %-30s %s\n", name, desc ? desc : "");
        }
        free(name); free(desc); free(ioid);
    }
    snd_device_name_free_hint(hints);
}

static void printRealtimeMetrics(const hexcaster::AudioEngine::RealtimeMetrics& metrics,
                                 unsigned int sampleRate, unsigned int frames)
{
    if (!metrics.enabled)
        return;

    const double deadlineUs = 1000000.0 * static_cast<double>(frames)
                           / static_cast<double>(sampleRate);
    const auto toUs = [](uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
    std::fprintf(stdout,
        "Realtime metrics (%llu blocks, deadline %.1f us):\n"
        "  DSP + conversion: mean %.1f us  p95 %.1f us  p99 %.1f us  p99.9 %.1f us  max %.1f us\n"
        "  DSP callback:     mean %.1f us  p95 %.1f us  p99 %.1f us  p99.9 %.1f us  max %.1f us\n"
        "  deadline misses:  %llu\n"
        "  capture overruns: %llu  playback underruns: %llu\n"
        "  short I/O:        reads=%llu writes=%llu\n"
        "  recoveries:       attempts=%llu failures=%llu\n",
        static_cast<unsigned long long>(metrics.processedBlocks), deadlineUs,
        toUs(metrics.processingMeanNs), toUs(metrics.processingP95Ns),
        toUs(metrics.processingP99Ns), toUs(metrics.processingP999Ns),
        toUs(metrics.processingMaxNs),
        toUs(metrics.callbackMeanNs), toUs(metrics.callbackP95Ns),
        toUs(metrics.callbackP99Ns), toUs(metrics.callbackP999Ns),
        toUs(metrics.callbackMaxNs),
        static_cast<unsigned long long>(metrics.deadlineMisses),
        static_cast<unsigned long long>(metrics.captureOverruns),
        static_cast<unsigned long long>(metrics.playbackUnderruns),
        static_cast<unsigned long long>(metrics.shortReads),
        static_cast<unsigned long long>(metrics.shortWrites),
        static_cast<unsigned long long>(metrics.recoveryAttempts),
        static_cast<unsigned long long>(metrics.recoveryFailures));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    std::fprintf(stdout, "HexCaster standalone  build: %s %s\n",
                 __DATE__, __TIME__);

    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage(argv[0]);
        return 1;
    }
    if (args.help)        { printUsage(argv[0]);  return 0; }
    if (args.listDevices) { listAlsaDevices();    return 0; }
    if (args.listMidi)    { listMidiDevices();    return 0; }

    if (args.modelPath.empty()) {
        std::fprintf(stderr, "Error: --model is required.\n\n");
        printUsage(argv[0]);
        return 1;
    }

    // -------------------------------------------------------------------------
    // Parameter registry and MIDI map
    // -------------------------------------------------------------------------

    hexcaster::ParamRegistry params;
    params.set(hexcaster::ParamId::MasterVolume_dB,       args.masterVolumeDb);
    params.set(hexcaster::ParamId::InputGain_dB,          args.gainDb);
    params.set(hexcaster::ParamId::NoiseGateThreshold_dB, args.gateThresholdDb);
    params.set(hexcaster::ParamId::HighShelfGain_dB,      args.eqHighShelfGainDb);
    params.set(hexcaster::ParamId::LowShelfGain_dB,       args.eqLowShelfGainDb);
    params.set(hexcaster::ParamId::BloomDepth_dB,          args.bloomDepth);
    params.set(hexcaster::ParamId::BloomCompensation,     args.bloomCompensation);

    hexcaster::MidiMap midiMap;
    for (const auto& m : args.midiMappings) {
        midiMap.map(m.cc, m.paramId);
        // Find the param name for display (linear scan over the same table
        // paramIdFromName uses -- only happens at startup, not in the audio path)
        const char* paramName = "?";
        struct { const char* n; hexcaster::ParamId id; } kNames[] = {
            {"InputGain_dB",          hexcaster::ParamId::InputGain_dB},
            {"BloomBasePre_dB",       hexcaster::ParamId::BloomBasePre_dB},
            {"BloomBasePost_dB",      hexcaster::ParamId::BloomBasePost_dB},
            {"BloomDepth_dB",         hexcaster::ParamId::BloomDepth_dB},
            {"BloomCompensation",     hexcaster::ParamId::BloomCompensation},
            {"BloomAttackMs",         hexcaster::ParamId::BloomAttackMs},
            {"BloomReleaseMs",        hexcaster::ParamId::BloomReleaseMs},
            {"BloomSensitivity_dB",   hexcaster::ParamId::BloomSensitivity_dB},
            {"NoiseGateThreshold_dB", hexcaster::ParamId::NoiseGateThreshold_dB},
            {"NoiseGateAttackMs",     hexcaster::ParamId::NoiseGateAttackMs},
            {"NoiseGateReleaseMs",    hexcaster::ParamId::NoiseGateReleaseMs},
            {"NoiseGateHoldMs",       hexcaster::ParamId::NoiseGateHoldMs},
            {"HighShelfHz",           hexcaster::ParamId::HighShelfHz},
            {"HighShelfGain_dB",      hexcaster::ParamId::HighShelfGain_dB},
            {"HighShelfBw",           hexcaster::ParamId::HighShelfBw},
            {"LowShelfHz",            hexcaster::ParamId::LowShelfHz},
            {"LowShelfGain_dB",       hexcaster::ParamId::LowShelfGain_dB},
            {"LowShelfBw",            hexcaster::ParamId::LowShelfBw},
            {"MasterVolume_dB",       hexcaster::ParamId::MasterVolume_dB},
        };
        for (auto& e : kNames)
            if (e.id == m.paramId) { paramName = e.n; break; }
        std::fprintf(stdout, "MIDI: CC %d -> %s\n", m.cc, paramName);
    }

    // -------------------------------------------------------------------------
    // DSP pipeline
    // -------------------------------------------------------------------------

    hexcaster::NoiseGate noiseGate;
    noiseGate.setThresholdDb(args.gateThresholdDb);

    hexcaster::GainStage inputGain;
    inputGain.setGainDb(args.gainDb);

    hexcaster::GainStage bloomPreGain;   // controlled by BloomController
    hexcaster::GainStage bloomPostGain;  // controlled by BloomController

    hexcaster::NamStage nam;
    nam.setInterfaceInputLevelDBu(args.inputLevelDBu);

    hexcaster::ShelfEQ eq;
    eq.setHighShelfGainDb(args.eqHighShelfGainDb);
    eq.setLowShelfGainDb (args.eqLowShelfGainDb);

    hexcaster::GainStage masterVolume;
    masterVolume.setGainDb(args.masterVolumeDb);

    // Bloom controller: drives bloomPreGain and bloomPostGain from
    // an envelope follower. Registered as a PipelineController -- it
    // runs in preProcess() before any stages execute.
    hexcaster::BloomController bloom(bloomPreGain, bloomPostGain);
    bloom.setDepth(args.bloomDepth);
    bloom.setCompensation(args.bloomCompensation);

    hexcaster::Pipeline pipeline;
    pipeline.addStage(&noiseGate);      // stage 0: noise gate
    pipeline.addStage(&inputGain);      // stage 1: model-relative user input trim
    pipeline.addStage(&bloomPreGain);   // stage 2: bloom pre-gain (dynamic)
    pipeline.addStage(&nam);            // stage 3: model calibration + amp model
    pipeline.addStage(&bloomPostGain);  // stage 4: bloom post-gain (dynamic)
    pipeline.addStage(&eq);             // stage 5: post-NAM EQ
    pipeline.addStage(&masterVolume);   // stage 6: master volume (user)
    pipeline.addController(&bloom);

    std::fprintf(stdout, "Pipeline: %d stage(s), %d controller(s)\n",
                 pipeline.numStages(), pipeline.numControllers());

    // -------------------------------------------------------------------------
    // Audio engine
    // -------------------------------------------------------------------------

    hexcaster::AudioEngine::Config audioConfig;
    audioConfig.inputDevice    = args.inputDevice;
    audioConfig.outputDevice   = args.outputDevice;
    audioConfig.sampleRate     = args.sampleRate;
    audioConfig.bufferFrames   = args.bufferFrames;
    audioConfig.periods        = 2;
    audioConfig.enableRealtimeMetrics = args.realtimeMetrics;
    audioConfig.inputChannel   = args.inputChannel;
    audioConfig.outputChannels = 0x3;

    hexcaster::AlsaAudioEngine engine;
    if (!engine.open(audioConfig)) {
        std::fprintf(stderr, "Error: %s\n", engine.errorMessage().c_str());
        return 1;
    }

    std::fprintf(stdout, "Audio: in=%s out=%s rate=%u frames=%u\n",
        audioConfig.inputDevice.c_str(),
        audioConfig.outputDevice.c_str(),
        engine.actualSampleRate(),
        engine.actualBufferFrames());

    if (engine.actualSampleRate() != args.sampleRate ||
        engine.actualBufferFrames() != args.bufferFrames) {
        std::fprintf(stdout,
            "Note: requested %u Hz / %u frames; ALSA negotiated %u Hz / %u frames\n",
            args.sampleRate, args.bufferFrames, engine.actualSampleRate(),
            engine.actualBufferFrames());
    }

    bool enableTuiObservations = false;
#ifdef HEXCASTER_TUI_ENABLED
    enableTuiObservations = args.tui;
#endif
    noiseGate.setObservationEnabled(enableTuiObservations);
    bloom.setObservationEnabled(enableTuiObservations);

    // ALSA negotiation is complete before the DSP graph and NAM loader are
    // prepared. This ensures every model sees the actual host rate and maximum
    // block size, never merely the requested values.
    pipeline.prepare(static_cast<float>(engine.actualSampleRate()),
                     static_cast<int>(engine.actualBufferFrames()));
    bloom.prepare(static_cast<float>(engine.actualSampleRate()),
                  static_cast<int>(engine.actualBufferFrames()));

    std::fprintf(stdout, "Loading model: %s\n", args.modelPath.c_str());
    if (!nam.loadModel(args.modelPath, args.namQuality)) {
        std::fprintf(stderr, "Error: failed to load model '%s'\n", args.modelPath.c_str());
        return 1;
    }

    // Trigger the pending model swap and touch model working state before the
    // audio thread is locked and promoted to realtime priority.
    {
        std::vector<float> warmup(engine.actualBufferFrames(), 0.f);
        pipeline.process(warmup.data(), static_cast<int>(warmup.size()));
    }

    const auto modelInfo = nam.modelInfo();
    std::fprintf(stdout,
        "Model loaded: %s  variant=%s version=%s engine=%s quality=%.2f rate=%.0f\n",
        modelInfo.path.c_str(), hexcaster::namModelVariantName(modelInfo.variant),
        modelInfo.version.c_str(), modelInfo.nativeStatic ? "NeuralAudio native" : "NAMCore/dynamic",
        modelInfo.selectedQuality, modelInfo.sampleRate);
    const float userInputTrimDb = params.get(hexcaster::ParamId::InputGain_dB);
    std::fprintf(stdout,
        "NAM input calibration:\n"
        "  interface: %+6.2f dBu\n",
        modelInfo.interfaceInputLevelDBu);
    if (modelInfo.hasInputCalibrationMetadata) {
        std::fprintf(stdout, "  model:     %+6.2f dBu (metadata)\n",
                     modelInfo.modelInputLevelDBu);
    } else {
        std::fprintf(stderr,
            "Warning: NAM model has no valid input_level_dbu metadata; "
            "using 0.00 dB automatic input calibration.\n");
        std::fprintf(stdout, "  model:       unavailable (fallback)\n");
    }
    std::fprintf(stdout,
        "  auto trim: %+6.2f dB\n"
        "  user trim: %+6.2f dB\n"
        "  effective: %+6.2f dB\n",
        modelInfo.inputCalibrationDb, userInputTrimDb,
        hexcaster::effectivePreNamGainDb(modelInfo.inputCalibrationDb,
                                        userInputTrimDb));

    // Signal level meters are needed by the TUI only. Headless production
    // avoids their full-buffer scans and logarithms.
    hexcaster::LevelMeter inputMeter;
    hexcaster::LevelMeter outputMeter;

    constexpr std::size_t kParamSlots = static_cast<std::size_t>(hexcaster::ParamId::kCount);
    std::array<float, kParamSlots> appliedParamValues{};
    std::array<bool, kParamSlots> hasAppliedParamValue{};
    auto applyParameterIfChanged = [&](hexcaster::ParamId id, auto&& apply) {
        const auto index = static_cast<std::size_t>(id);
        const float value = params.get(id);
        if (!hasAppliedParamValue[index] || value != appliedParamValues[index]) {
            appliedParamValues[index] = value;
            hasAppliedParamValue[index] = true;
            apply(value);
        }
    };

    // Audio callback: atomic parameters are sampled once per block. Stage
    // setters run only after a parameter change, avoiding steady-state dB and
    // coefficient conversions while preserving the existing smoothing.
    engine.setCallback([&](float* buf, int n) {
        applyParameterIfChanged(hexcaster::ParamId::NoiseGateThreshold_dB,
                                [&](float value) { noiseGate.setThresholdDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::NoiseGateAttackMs,
                                [&](float value) { noiseGate.setAttackMs(value); });
        applyParameterIfChanged(hexcaster::ParamId::NoiseGateReleaseMs,
                                [&](float value) { noiseGate.setReleaseMs(value); });
        applyParameterIfChanged(hexcaster::ParamId::NoiseGateHoldMs,
                                [&](float value) { noiseGate.setHoldMs(value); });
        applyParameterIfChanged(hexcaster::ParamId::InputGain_dB,
                                [&](float value) { inputGain.setGainDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomBasePre_dB,
                                [&](float value) { bloom.setBasePreDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomBasePost_dB,
                                [&](float value) { bloom.setBasePostDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomDepth_dB,
                                [&](float value) { bloom.setDepth(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomCompensation,
                                [&](float value) { bloom.setCompensation(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomAttackMs,
                                [&](float value) { bloom.setAttackMs(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomReleaseMs,
                                [&](float value) { bloom.setReleaseMs(value); });
        applyParameterIfChanged(hexcaster::ParamId::BloomSensitivity_dB,
                                [&](float value) { bloom.setSensitivity(value); });
        applyParameterIfChanged(hexcaster::ParamId::HighShelfGain_dB,
                                [&](float value) { eq.setHighShelfGainDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::HighShelfHz,
                                [&](float value) { eq.setHighShelfHz(value); });
        applyParameterIfChanged(hexcaster::ParamId::HighShelfBw,
                                [&](float value) { eq.setHighShelfBw(value); });
        applyParameterIfChanged(hexcaster::ParamId::LowShelfGain_dB,
                                [&](float value) { eq.setLowShelfGainDb(value); });
        applyParameterIfChanged(hexcaster::ParamId::LowShelfHz,
                                [&](float value) { eq.setLowShelfHz(value); });
        applyParameterIfChanged(hexcaster::ParamId::LowShelfBw,
                                [&](float value) { eq.setLowShelfBw(value); });
        applyParameterIfChanged(hexcaster::ParamId::MasterVolume_dB,
                                [&](float value) { masterVolume.setGainDb(value); });
        if (enableTuiObservations)
            inputMeter.measure(buf, n);
        pipeline.process(buf, n);
        if (enableTuiObservations)
            outputMeter.measure(buf, n);
    });

    // -------------------------------------------------------------------------
    // MIDI input (optional)
    // -------------------------------------------------------------------------

    hexcaster::MidiInput midiInput;

    if (!args.midiDevice.empty()) {
        if (!midiInput.open(args.midiDevice)) {
            std::fprintf(stderr, "Warning: %s\n  Continuing without MIDI.\n",
                         midiInput.errorMessage().c_str());
        } else {
            midiInput.start(midiMap, params);
        }
    }

    // -------------------------------------------------------------------------
    // Signal handler
    // -------------------------------------------------------------------------

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // -------------------------------------------------------------------------
    // TUI mode vs headless mode
    // -------------------------------------------------------------------------

#ifdef HEXCASTER_TUI_ENABLED
    if (args.tui) {
        // TUI mode: audio engine runs on a background thread,
        // TUI (FTXUI) runs on the main thread.

        // Snapshot producer: called ~30x/s on the TUI refresh thread.
        // All reads are atomic -- no locks, real-time safe.
        auto snapshotFn = [&]() -> hexcaster::tui::MeterData {
            hexcaster::tui::MeterData d;
            d.modelName            = nam.modelPath();
            d.gateGain             = noiseGate.getGateGain();
            d.gateState            = static_cast<int>(noiseGate.getState());
            d.noiseGateThreshold   = params.get(hexcaster::ParamId::NoiseGateThreshold_dB);
            d.noiseGateAttack      = params.get(hexcaster::ParamId::NoiseGateAttackMs);
            d.noiseGateRelease     = params.get(hexcaster::ParamId::NoiseGateReleaseMs);
            d.noiseGateHold        = params.get(hexcaster::ParamId::NoiseGateHoldMs);
            d.inputGain            = params.get(hexcaster::ParamId::InputGain_dB);
            d.modelCalibration     = modelInfo.inputCalibrationDb;
            d.effectiveInputGain   = hexcaster::effectivePreNamGainDb(
                d.modelCalibration, d.inputGain);
            d.masterVolume         = params.get(hexcaster::ParamId::MasterVolume_dB);
            d.bloomDetectorEnv       = bloom.getDetectorEnvelope();
            d.bloomEnvelope          = bloom.getGainEnvelope();
            d.bloomChordScore        = bloom.getChordScore();
            d.bloomBasePre           = params.get(hexcaster::ParamId::BloomBasePre_dB);
            d.bloomBasePost          = params.get(hexcaster::ParamId::BloomBasePost_dB);
            d.bloomPreGainApplied    = bloomPreGain.getGainDb();
            d.bloomPostGainApplied   = bloomPostGain.getGainDb();
            d.bloomDepth           = params.get(hexcaster::ParamId::BloomDepth_dB);
            d.bloomCompensation    = params.get(hexcaster::ParamId::BloomCompensation);
            d.bloomSensitivity     = params.get(hexcaster::ParamId::BloomSensitivity_dB);
            d.bloomAttack          = params.get(hexcaster::ParamId::BloomAttackMs);
            d.bloomRelease         = params.get(hexcaster::ParamId::BloomReleaseMs);
            d.eqHighShelfGain      = params.get(hexcaster::ParamId::HighShelfGain_dB);
            d.eqLowShelfGain       = params.get(hexcaster::ParamId::LowShelfGain_dB);
            d.inputLevelDb         = inputMeter.getPeakDb();
            d.outputLevelDb        = outputMeter.getPeakDb();
            return d;
        };

        // Run the audio engine on a background thread.
        // The thread is given SCHED_FIFO priority inside engine.run().
        std::atomic<bool> audioFailed{false};
        std::thread audioThread([&]() {
            engine.run();
            if (!gQuit.load(std::memory_order_relaxed)) {
                audioFailed.store(true, std::memory_order_relaxed);
                gQuit.store(true, std::memory_order_relaxed);
            }
        });

        // Run TUI on the main thread (FTXUI owns the terminal here).
        {
            hexcaster::tui::Tui tui(snapshotFn, params, midiMap, gQuit);
            tui.run();
        }

        // TUI has exited -- stop audio and clean up
        engine.stop();
        audioThread.join();

        printRealtimeMetrics(engine.realtimeMetrics(), engine.actualSampleRate(),
                             engine.actualBufferFrames());

        if (audioFailed.load(std::memory_order_relaxed)) {
            runtimeFailed = true;
            std::fprintf(stderr, "Audio engine failed: %s\n",
                         engine.errorMessage().c_str());
        }

    } else
#endif // HEXCASTER_TUI_ENABLED
    {
        // Headless mode: audio engine runs on the main thread (unchanged behavior).
        std::fprintf(stdout,
            "Running -- press Ctrl+C to stop.\n"
            "Gate: %.1f dB  |  Input gain: %.1f dB  |  Input ch: %d  |  Output: L+R%s\n",
            args.gateThresholdDb, args.gainDb, args.inputChannel,
            midiInput.isOpen() ? "  |  MIDI active" : "");

        std::thread watcher([&]() {
            uint64_t reportedIoErrors = 0;
            int statusPolls = 0;
            while (!gQuit.load(std::memory_order_relaxed)) {
                usleep(50000);
                if (++statusPolls < 10)
                    continue;
                statusPolls = 0;
                const auto io = engine.ioStatus();
                if (io.errors != reportedIoErrors) {
                    reportedIoErrors = io.errors;
                    std::fprintf(stderr,
                        "ALSA runtime I/O error: side=%s error=%s total=%llu "
                        "recoveries=%llu failures=%llu\n",
                        io.lastWasCapture ? "capture" : "playback",
                        snd_strerror(io.lastErrorCode),
                        static_cast<unsigned long long>(io.errors),
                        static_cast<unsigned long long>(io.recoveryAttempts),
                        static_cast<unsigned long long>(io.recoveryFailures));
                }
            }
            engine.stop();
        });

        engine.run();
        printRealtimeMetrics(engine.realtimeMetrics(), engine.actualSampleRate(),
                             engine.actualBufferFrames());
        const bool audioFailed = !gQuit.load(std::memory_order_relaxed);
        gQuit.store(true, std::memory_order_relaxed);
        watcher.join();

        if (audioFailed) {
            runtimeFailed = true;
            std::fprintf(stderr, "Audio engine failed: %s\n",
                         engine.errorMessage().c_str());
        }
    }

    // -------------------------------------------------------------------------
    // Shared shutdown sequence (both modes)
    // -------------------------------------------------------------------------

    midiInput.stop();
    midiInput.close();
    engine.close();

    std::fprintf(stdout, "Bye.\n");
    return runtimeFailed ? 1 : 0;
}
