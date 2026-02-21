#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/param_registry.h"

#include <cstdio>

// audio_engine.h will wrap ALSA/JACK/PipeWire -- stub for now.
// #include "audio_engine.h"

int main(int /*argc*/, char** /*argv*/)
{
    std::printf("HexCaster standalone runtime\n");
    std::printf("Build: " __DATE__ " " __TIME__ "\n");

    // Parameter store
    hexcaster::ParamRegistry params;

    // Stages
    hexcaster::GainStage masterGain;
    masterGain.setGainDb(params.get(hexcaster::ParamId::MasterGain_dB));

    // Pipeline
    hexcaster::Pipeline pipeline;
    pipeline.addStage(&masterGain);
    pipeline.prepare(48000.f, 128);

    std::printf("Pipeline ready: %d stage(s).\n", pipeline.numStages());
    std::printf("Audio engine not yet connected (JACK/ALSA stub).\n");

    // TODO: start audio_engine, hand off pipeline.process() as callback

    return 0;
}
