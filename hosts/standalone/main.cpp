#include "audio_engine.h"
#include "alsa_audio_engine.h"
#include "midi_input.h"

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/nam_stage.h"
#include "hexcaster/noise_gate.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"
#include "hexcaster/param_id.h"

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
    int          inputChannel         = 0;
    bool         listDevices    = false;
    bool         listMidi       = false;
    bool         help           = false;
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
        "  --input-channel <N>         Capture channel: 0=left, 1=right  [default: 0]\n"
        "  --midi-device <hw:X,Y,Z>    ALSA raw MIDI input device\n"
        "  --midi-cc <cc>:<ParamName>  Map a MIDI CC to a parameter  (repeatable)\n"
        "  --list-devices              Print ALSA PCM devices and exit\n"
        "  --list-midi                 Print ALSA raw MIDI devices and exit\n"
        "  --help                      Show this help and exit\n"
        "\n"
        "Parameter names for --midi-cc:\n"
        "  InputGain_dB         BloomBasePre_dB    BloomBasePost_dB\n"
        "  BloomPreDepth        BloomPostDepth     EnvAttackMs  EnvReleaseMs\n"
        "  NoiseGateThreshold_dB  NoiseGateAttackMs  NoiseGateReleaseMs  NoiseGateHoldMs\n"
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
            {"BloomPreDepth",         hexcaster::ParamId::BloomPreDepth},
            {"BloomPostDepth",        hexcaster::ParamId::BloomPostDepth},
            {"EnvAttackMs",           hexcaster::ParamId::EnvAttackMs},
            {"EnvReleaseMs",          hexcaster::ParamId::EnvReleaseMs},
            {"NoiseGateThreshold_dB", hexcaster::ParamId::NoiseGateThreshold_dB},
            {"NoiseGateAttackMs",     hexcaster::ParamId::NoiseGateAttackMs},
            {"NoiseGateReleaseMs",    hexcaster::ParamId::NoiseGateReleaseMs},
            {"NoiseGateHoldMs",       hexcaster::ParamId::NoiseGateHoldMs},
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

    hexcaster::NamStage nam;

    hexcaster::Pipeline pipeline;
    pipeline.addStage(&noiseGate);   // stage 0: gate first
    pipeline.addStage(&inputGain);   // stage 1: input gain
    pipeline.addStage(&nam);         // stage 2: amp model
    pipeline.prepare(static_cast<float>(args.sampleRate),
                     static_cast<int>(args.bufferFrames));

    std::fprintf(stdout, "Pipeline: %d stage(s)\n", pipeline.numStages());

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
    }

    // Audio callback: sync params -> stages each block, then process.
    // Param reads are atomic; no locks in this path.
    engine.setCallback([&](float* buf, int n) {
        // Sync params -> stages each block. Reads are atomic; no locks.
        noiseGate.setThresholdDb(params.get(hexcaster::ParamId::NoiseGateThreshold_dB));
        noiseGate.setAttackMs   (params.get(hexcaster::ParamId::NoiseGateAttackMs));
        noiseGate.setReleaseMs  (params.get(hexcaster::ParamId::NoiseGateReleaseMs));
        noiseGate.setHoldMs     (params.get(hexcaster::ParamId::NoiseGateHoldMs));
        inputGain.setGainDb     (params.get(hexcaster::ParamId::InputGain_dB));
        pipeline.process(buf, n);
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
    // Signal handler + run
    // -------------------------------------------------------------------------

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

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

    // Shutdown sequence: stop MIDI before audio is fully torn down
    midiInput.stop();
    midiInput.close();

    watcher.join();
    engine.close();

    std::fprintf(stdout, "Bye.\n");
    return 0;
}
