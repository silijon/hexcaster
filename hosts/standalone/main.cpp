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

#ifdef HEXCASTER_TUI_ENABLED
#include "tui/tui.h"
#endif

#include "level_meter.h"

#include <atomic>
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
    std::string  midiDevice;                    // empty = MIDI disabled
    unsigned int sampleRate     = 48000;
    unsigned int bufferFrames   = 128;
    float        gainDb                = 0.f;
    float        gateThresholdDb      = -60.f;
    float        eqGainDb             = 0.f;
    float        eqSweepHz            = 1000.f;
    float        masterVolumeDb       = 0.f;
    float        bloomDepth           = 6.f;
    float        bloomCompensation    = 0.5f;
    int          inputChannel         = 0;
    bool         listDevices    = false;
    bool         listMidi       = false;
    bool         help           = false;
    bool         tui            = false;
    std::vector<MidiCcMapping> midiMappings;
};

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s --model <path.nam> [options]\n"
        "\n"
        "Options:\n"
        "  --model <path>              NAM model file (.nam)  [required]\n"
        "  --device <hw:X,Y>           Set both input and output device\n"
        "  --input-device <dev>        Input audio device\n"
        "  --output-device <dev>       Output audio device\n"
        "  --sample-rate <Hz>          Sample rate  [default: 48000]\n"
        "  --buffer <frames>           Buffer size in frames  [default: 128]\n"
        "  --gain <dB>                 Initial input gain in dB  [default: 0.0]\n"
        "  --gate-threshold <dB>       Noise gate threshold  [-80, 0] dB  [default: -60]\n"
        "  --eq-gain <dB>              Post-NAM EQ gain  [-12, +12] dB  [default: 0]\n"
        "  --eq-sweep <Hz>             Post-NAM EQ center frequency  [300, 2500] Hz  [default: 1000]\n"
        "  --master-volume <dB>        Final output level to power amp  [-60, +24] dB  [default: 0]\n"
        "  --bloom-depth <dB>          Bloom max input gain reduction  [0, 24] dB  [default: 6]\n"
        "  --bloom-compensation <r>    Bloom output compensation ratio  [0, 2]  [default: 0.5]\n"
        "  --input-channel <N>         Capture channel: 0=left, 1=right  [default: 0]\n"
        "  --midi-device <hw:X,Y,Z>    ALSA raw MIDI input device\n"
        "  --midi-cc <cc>:<ParamName>  Map a MIDI CC to a parameter  (repeatable)\n"
        "  --list-devices              Print ALSA PCM devices and exit\n"
        "  --list-midi                 Print ALSA raw MIDI devices and exit\n"
        "  --tui                       Start in terminal UI mode\n"
        "  --help                      Show this help and exit\n"
        "\n"
        "Parameter names for --midi-cc:\n"
        "  InputGain_dB         BloomBasePre_dB    BloomBasePost_dB\n"
        "  BloomDepth_dB  BloomCompensation  BloomSensitivity_dB  BloomAttackMs  BloomReleaseMs\n"
        "  NoiseGateThreshold_dB  NoiseGateAttackMs  NoiseGateReleaseMs  NoiseGateHoldMs\n"
        "  EqGain_dB  EqSweepHz  EqQ  MasterVolume_dB\n"
        "\n"
        "Examples:\n"
        "  %s --model ~/amp.nam --input-device hw:CARD=V276,DEV=0 \\\n"
        "     --output-device hw:CARD=sndrpihifiberry,DEV=0\n"
        "\n"
        "  %s --model ~/amp.nam --input-device hw:CARD=V276,DEV=0 \\\n"
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

        if (std::strcmp(key, "--model") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.modelPath = v;
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
        } else if (std::strcmp(key, "--gate-threshold") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.gateThresholdDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--eq-gain") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.eqGainDb = static_cast<float>(std::atof(v));
        } else if (std::strcmp(key, "--eq-sweep") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.eqSweepHz = static_cast<float>(std::atof(v));
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
    params.set(hexcaster::ParamId::InputGain_dB,          args.gainDb);
    params.set(hexcaster::ParamId::NoiseGateThreshold_dB, args.gateThresholdDb);
    params.set(hexcaster::ParamId::EqGain_dB,             args.eqGainDb);
    params.set(hexcaster::ParamId::EqSweepHz,             args.eqSweepHz);
    params.set(hexcaster::ParamId::MasterVolume_dB,       args.masterVolumeDb);
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
            {"EqGain_dB",             hexcaster::ParamId::EqGain_dB},
            {"EqSweepHz",             hexcaster::ParamId::EqSweepHz},
            {"EqQ",                   hexcaster::ParamId::EqQ},
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

    hexcaster::MidSweepEQ eq;
    eq.setGainDb (args.eqGainDb);
    eq.setSweepHz(args.eqSweepHz);

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
    pipeline.addStage(&inputGain);      // stage 1: input gain (user)
    pipeline.addStage(&bloomPreGain);   // stage 2: bloom pre-gain (dynamic)
    pipeline.addStage(&nam);            // stage 3: amp model
    pipeline.addStage(&bloomPostGain);  // stage 4: bloom post-gain (dynamic)
    pipeline.addStage(&eq);             // stage 5: post-NAM EQ
    pipeline.addStage(&masterVolume);   // stage 6: master volume (user)
    pipeline.addController(&bloom);
    pipeline.prepare(static_cast<float>(args.sampleRate),
                     static_cast<int>(args.bufferFrames));
    bloom.prepare(static_cast<float>(args.sampleRate),
                  static_cast<int>(args.bufferFrames));

    std::fprintf(stdout, "Pipeline: %d stage(s), %d controller(s)\n",
                 pipeline.numStages(), pipeline.numControllers());

    // -------------------------------------------------------------------------
    // Load NAM model
    // -------------------------------------------------------------------------

    std::fprintf(stdout, "Loading model: %s\n", args.modelPath.c_str());
    if (!nam.loadModel(args.modelPath)) {
        std::fprintf(stderr, "Error: failed to load model '%s'\n", args.modelPath.c_str());
        return 1;
    }

    // Warm-up block: triggers the pending model swap before the audio thread starts
    {
        std::vector<float> warmup(args.bufferFrames, 0.f);
        pipeline.process(warmup.data(), static_cast<int>(args.bufferFrames));
    }

    std::fprintf(stdout, "Model loaded: %s\n", nam.modelPath().c_str());

    // -------------------------------------------------------------------------
    // Audio engine
    // -------------------------------------------------------------------------

    hexcaster::AudioEngine::Config audioConfig;
    audioConfig.inputDevice    = args.inputDevice;
    audioConfig.outputDevice   = args.outputDevice;
    audioConfig.sampleRate     = args.sampleRate;
    audioConfig.bufferFrames   = args.bufferFrames;
    audioConfig.periods        = 2;
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

    if (engine.actualBufferFrames() != args.bufferFrames) {
        std::fprintf(stdout, "Note: requested %u frames, device gave %u\n",
            args.bufferFrames, engine.actualBufferFrames());
        pipeline.prepare(static_cast<float>(engine.actualSampleRate()),
                         static_cast<int>(engine.actualBufferFrames()));
        bloom.prepare(static_cast<float>(engine.actualSampleRate()),
                      static_cast<int>(engine.actualBufferFrames()));
    }

    // Signal level meters -- measured in the audio callback, read by TUI.
    hexcaster::LevelMeter inputMeter;
    hexcaster::LevelMeter outputMeter;

    // Audio callback: sync params -> stages each block, then process.
    // Param reads are atomic; no locks in this path.
    engine.setCallback([&](float* buf, int n) {
        // Sync params -> stages each block. Reads are atomic; no locks.
        noiseGate.setThresholdDb(params.get(hexcaster::ParamId::NoiseGateThreshold_dB));
        noiseGate.setAttackMs   (params.get(hexcaster::ParamId::NoiseGateAttackMs));
        noiseGate.setReleaseMs  (params.get(hexcaster::ParamId::NoiseGateReleaseMs));
        noiseGate.setHoldMs     (params.get(hexcaster::ParamId::NoiseGateHoldMs));
        inputGain.setGainDb     (params.get(hexcaster::ParamId::InputGain_dB));
        bloom.setBasePreDb      (params.get(hexcaster::ParamId::BloomBasePre_dB));
        bloom.setBasePostDb     (params.get(hexcaster::ParamId::BloomBasePost_dB));
        bloom.setDepth          (params.get(hexcaster::ParamId::BloomDepth_dB));
        bloom.setCompensation   (params.get(hexcaster::ParamId::BloomCompensation));
        bloom.setAttackMs       (params.get(hexcaster::ParamId::BloomAttackMs));
        bloom.setReleaseMs      (params.get(hexcaster::ParamId::BloomReleaseMs));
        bloom.setSensitivity    (params.get(hexcaster::ParamId::BloomSensitivity_dB));
        eq.setGainDb            (params.get(hexcaster::ParamId::EqGain_dB));
        eq.setSweepHz           (params.get(hexcaster::ParamId::EqSweepHz));
        eq.setQ                 (params.get(hexcaster::ParamId::EqQ));
        masterVolume.setGainDb  (params.get(hexcaster::ParamId::MasterVolume_dB));
        inputMeter.measure(buf, n);
        pipeline.process(buf, n);
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
            d.masterVolume         = params.get(hexcaster::ParamId::MasterVolume_dB);
            d.bloomEnvelope          = bloom.getEnvelope();
            d.bloomBasePre           = params.get(hexcaster::ParamId::BloomBasePre_dB);
            d.bloomBasePost          = params.get(hexcaster::ParamId::BloomBasePost_dB);
            d.bloomPreGainApplied    = bloomPreGain.getGainDb();
            d.bloomPostGainApplied   = bloomPostGain.getGainDb();
            d.bloomDepth           = params.get(hexcaster::ParamId::BloomDepth_dB);
            d.bloomCompensation    = params.get(hexcaster::ParamId::BloomCompensation);
            d.bloomSensitivity     = params.get(hexcaster::ParamId::BloomSensitivity_dB);
            d.bloomAttack          = params.get(hexcaster::ParamId::BloomAttackMs);
            d.bloomRelease         = params.get(hexcaster::ParamId::BloomReleaseMs);
            d.eqGain               = params.get(hexcaster::ParamId::EqGain_dB);
            d.eqSweep              = params.get(hexcaster::ParamId::EqSweepHz);
            d.eqQ                  = params.get(hexcaster::ParamId::EqQ);
            d.inputLevelDb         = inputMeter.getPeakDb();
            d.outputLevelDb        = outputMeter.getPeakDb();
            return d;
        };

        // Run the audio engine on a background thread.
        // The thread is given SCHED_FIFO priority inside engine.run().
        std::thread audioThread([&]() {
            engine.run();
        });

        // Run TUI on the main thread (FTXUI owns the terminal here).
        {
            hexcaster::tui::Tui tui(snapshotFn, params, midiMap, gQuit);
            tui.run();
        }

        // TUI has exited -- stop audio and clean up
        engine.stop();
        audioThread.join();

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
            while (!gQuit.load(std::memory_order_relaxed))
                usleep(50000);
            engine.stop();
        });

        engine.run();
        watcher.join();
    }

    // -------------------------------------------------------------------------
    // Shared shutdown sequence (both modes)
    // -------------------------------------------------------------------------

    midiInput.stop();
    midiInput.close();
    engine.close();

    std::fprintf(stdout, "Bye.\n");
    return 0;
}
