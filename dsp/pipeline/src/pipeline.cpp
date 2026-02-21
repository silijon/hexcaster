#include "hexcaster/pipeline.h"
#include <cassert>

namespace hexcaster {

void Pipeline::addStage(ProcessorStage* stage)
{
    assert(numStages_ < kMaxStages && "Pipeline stage limit exceeded");
    assert(stage != nullptr);
    stages_[numStages_++] = stage;
}

void Pipeline::addController(PipelineController* controller)
{
    assert(numControllers_ < kMaxControllers && "Pipeline controller limit exceeded");
    assert(controller != nullptr);
    controllers_[numControllers_++] = controller;
}

void Pipeline::prepare(float sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    for (int i = 0; i < numStages_; ++i) {
        stages_[i]->prepare(sampleRate, maxBlockSize);
    }
}

void Pipeline::process(float* buffer, int numSamples)
{
    // 1. Notify controllers before any stages run
    for (int c = 0; c < numControllers_; ++c) {
        controllers_[c]->preProcess(buffer, numSamples);
    }

    // 2. Process stages in order, notifying controllers between each
    for (int s = 0; s < numStages_; ++s) {
        stages_[s]->process(buffer, numSamples);

        for (int c = 0; c < numControllers_; ++c) {
            controllers_[c]->betweenStages(s, buffer, numSamples);
        }
    }
}

void Pipeline::reset()
{
    for (int i = 0; i < numStages_; ++i) {
        stages_[i]->reset();
    }
}

} // namespace hexcaster
