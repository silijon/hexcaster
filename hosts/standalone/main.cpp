#include "audio_engine.h"
#include "alsa_audio_engine.h"

#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/nam_stage.h"
#include "hexcaster/param_registry.h"

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
// Signal handling -- Ctrl+C sets this flag, audio loop checks it
// ---------------------------------------------------------------------------

static std::atomic<bool> gQuit{ false };

static void handleSignal(int /*sig*/)
{
    gQuit.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

struct Args {
    std::string inputDevice   = "hw:2,0";
    std::string outputDevice  = "hw:2,0";
    std::string modelPath;
    unsigned int sampleRate   = 48000;
    unsigned int bufferFrames = 128;
    float        gainDb       = 0.f;
    int          inputChannel = 0;   // 0=left, 1=right
    bool         listDevices  = false;
    bool         help         = false;
};

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s --model <path.nam> [options]\n"
        "\n"
        "Options:\n"
        "  --model <path>         Path to NAM model file (.nam)  [required]\n"
        "  --device <hw:X,Y>      Set both input and output device  [default: hw:2,0]\n"
        "  --input-device <hw:>   Input device (overrides --device)\n"
        "  --output-device <hw:>  Output device (overrides --device)\n"
        "  --sample-rate <Hz>     Sample rate  [default: 48000]\n"
        "  --buffer <frames>      Buffer size in frames  [default: 128]\n"
        "  --gain <dB>            Master gain in dB  [default: 0.0]\n"
        "  --input-channel <N>    Capture channel index: 0=left, 1=right  [default: 0]\n"
        "  --list-devices         Print ALSA PCM devices and exit\n"
        "  --help                 Show this help and exit\n"
        "\n"
        "Examples:\n"
        "  %s --model ~/models/amp.nam\n"
        "  %s --model ~/amp.nam --device hw:1,0 --buffer 64\n"
        "  %s --model ~/amp.nam --input-device hw:2,0 --output-device hw:3,0\n",
        prog, prog, prog, prog);
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
        } else if (std::strcmp(key, "--input-channel") == 0) {
            const char* v = nextArg(); if (!v) return false;
            args.inputChannel = std::atoi(v);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", key);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Device listing (handy for finding hw:X,Y numbers on the Pi)
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

        // Only show hardware devices and those with no IOID filter (both)
        if (name && (ioid == nullptr ||
                     std::strcmp(ioid, "Input") == 0 ||
                     std::strcmp(ioid, "Output") == 0)) {
            std::fprintf(stdout, "  %-30s %s\n",
                         name, desc ? desc : "");
        }

        free(name);
        free(desc);
        free(ioid);
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
    if (args.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (args.listDevices) {
        listAlsaDevices();
        return 0;
    }
    if (args.modelPath.empty()) {
        std::fprintf(stderr, "Error: --model is required.\n\n");
        printUsage(argv[0]);
        return 1;
    }

    // -------------------------------------------------------------------------
    // Build the DSP pipeline
    // -------------------------------------------------------------------------

    hexcaster::ParamRegistry params;
    params.set(hexcaster::ParamId::MasterGain_dB, args.gainDb);

    hexcaster::GainStage masterGain;
    masterGain.setGainDb(args.gainDb);

    hexcaster::NamStage nam;

    hexcaster::Pipeline pipeline;
    pipeline.addStage(&masterGain);
    pipeline.addStage(&nam);
    pipeline.prepare(static_cast<float>(args.sampleRate),
                     static_cast<int>(args.bufferFrames));

    std::fprintf(stdout, "Pipeline: %d stage(s)\n", pipeline.numStages());

    // -------------------------------------------------------------------------
    // Load NAM model (blocking -- happens before audio engine starts)
    // -------------------------------------------------------------------------

    std::fprintf(stdout, "Loading model: %s\n", args.modelPath.c_str());
    bool modelOk = nam.loadModel(args.modelPath);

    if (!modelOk) {
        std::fprintf(stderr, "Error: failed to load model '%s'\n",
                     args.modelPath.c_str());
        return 1;
    }

    // Trigger the pending swap by running one silent block (off-thread, safe here)
    {
        std::vector<float> warmup(args.bufferFrames, 0.f);
        pipeline.process(warmup.data(), static_cast<int>(args.bufferFrames));
    }

    std::fprintf(stdout, "Model loaded: %s\n", nam.modelPath().c_str());

    // -------------------------------------------------------------------------
    // Open audio engine
    // -------------------------------------------------------------------------

    hexcaster::AudioEngine::Config audioConfig;
    audioConfig.inputDevice   = args.inputDevice;
    audioConfig.outputDevice  = args.outputDevice;
    audioConfig.sampleRate    = args.sampleRate;
    audioConfig.bufferFrames  = args.bufferFrames;
    audioConfig.periods       = 2;
    audioConfig.inputChannel  = args.inputChannel;
    audioConfig.outputChannels = 0x3;  // both L+R

    hexcaster::AlsaAudioEngine engine;

    if (!engine.open(audioConfig)) {
        std::fprintf(stderr, "Error: %s\n", engine.errorMessage().c_str());
        return 1;
    }

    std::fprintf(stdout,
        "Audio: in=%s out=%s rate=%u frames=%u\n",
        audioConfig.inputDevice.c_str(),
        audioConfig.outputDevice.c_str(),
        engine.actualSampleRate(),
        engine.actualBufferFrames());

    // Warn if device negotiated a different buffer size than requested
    if (engine.actualBufferFrames() != args.bufferFrames) {
        std::fprintf(stdout,
            "Note: requested %u frames, device gave %u\n",
            args.bufferFrames, engine.actualBufferFrames());
        // Re-prepare pipeline with the actual frame count
        pipeline.prepare(static_cast<float>(engine.actualSampleRate()),
                         static_cast<int>(engine.actualBufferFrames()));
    }

    // Wire the pipeline as the audio callback
    engine.setCallback([&](float* buf, int n) {
        pipeline.process(buf, n);
    });

    // -------------------------------------------------------------------------
    // Install signal handler and run
    // -------------------------------------------------------------------------

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::fprintf(stdout,
        "Running -- press Ctrl+C to stop.\n"
        "Gain: %.1f dB  |  Input ch: %d  |  Output: L+R\n",
        args.gainDb, args.inputChannel);

    // Spin a watcher thread that calls engine.stop() when gQuit is set.
    // This lets run() (which blocks) be interrupted cleanly without
    // calling stop() from inside the signal handler.
    std::thread watcher([&]() {
        while (!gQuit.load(std::memory_order_relaxed))
            usleep(50000);  // 50ms poll -- not RT, just for shutdown
        engine.stop();
    });

    engine.run();  // blocks until stop() is called

    watcher.join();
    engine.close();

    std::fprintf(stdout, "Bye.\n");
    return 0;
}
