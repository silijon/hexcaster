#pragma once

#include "hexcaster/processor_stage.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Forward-declare to avoid pulling NeuralAudio headers into consumer code
namespace NeuralAudio { class NeuralModel; }

namespace hexcaster {

/**
 * NamStage: ProcessorStage wrapper around NeuralAudio::NeuralModel.
 *
 * Responsibilities:
 *   - Load a .nam model file at initialization time (never during process())
 *   - Delegate per-block inference to NeuralModel::Process()
 *   - Apply the model's recommended input/output dB adjustments
 *   - Expose a pending model path that can be set from the control thread
 *     and swapped safely between blocks (not sample-accurate, but safe)
 *
 * Real-time safety:
 *   - process() is RT-safe: no allocation, no file I/O, bounded time
 *   - loadModel() is NOT RT-safe: call from control/init thread only
 *   - pendingModelPath_ is checked at the top of process() -- if a new
 *     path has been set, the model is swapped before processing the block.
 *     This swap does allocate (std::unique_ptr swap), so it is not strictly
 *     RT-safe, but it is bounded and infrequent. A lock-free double-buffer
 *     can replace this later if needed.
 *
 * Usage:
 *   NamStage nam;
 *   nam.prepare(48000.f, 128);
 *   nam.loadModel("/path/to/model.nam");  // control thread
 *   // audio thread:
 *   nam.process(buffer, numSamples);
 */
class NamStage : public ProcessorStage {
public:
    NamStage();
    ~NamStage() override;

    // Non-copyable (owns the model)
    NamStage(const NamStage&)            = delete;
    NamStage& operator=(const NamStage&) = delete;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    /**
     * Load a model from the given file path. Not real-time safe.
     * May be called from the control thread at any time; the model will
     * be swapped in at the start of the next process() block.
     *
     * Returns true on success. On failure, the previous model (if any)
     * remains active.
     */
    bool loadModel(const std::string& path);

    /**
     * Unload the current model. Not real-time safe.
     * After this, process() will pass audio through unmodified.
     */
    void unloadModel();

    bool hasModel() const;
    const std::string& modelPath() const { return currentModelPath_; }

private:
    std::unique_ptr<NeuralAudio::NeuralModel> model_;
    std::string currentModelPath_;

    // Pending model set by control thread, swapped in at top of process()
    std::unique_ptr<NeuralAudio::NeuralModel> pendingModel_;
    std::string pendingModelPath_;
    std::atomic<bool> modelPending_{ false };

    // Working buffer for NeuralAudio output (pre-allocated in prepare())
    std::vector<float> outputBuffer_;

    int   maxBlockSize_ = 0;
    float sampleRate_   = 0.f;

    // Calibration offsets from the model (applied as linear gain)
    float inputGainLinear_  = 1.f;
    float outputGainLinear_ = 1.f;

    void applyPendingModel();
    void updateCalibration();
};

} // namespace hexcaster
