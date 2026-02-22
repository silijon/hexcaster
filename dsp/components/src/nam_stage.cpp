#include "hexcaster/nam_stage.h"
#include "NeuralAudio/NeuralModel.h"

#include <cmath>
#include <cstring>

namespace hexcaster {

NamStage::NamStage() = default;
NamStage::~NamStage() = default;

void NamStage::prepare(float sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    // Set the global max buffer size before any model is used.
    // Not RT-safe -- must be called before the audio thread starts.
    NeuralAudio::NeuralModel::SetDefaultMaxAudioBufferSize(maxBlockSize);

    // Pre-allocate the output buffer NeuralAudio writes into.
    outputBuffer_.assign(static_cast<std::size_t>(maxBlockSize), 0.f);

    // If a model was already loaded, update its buffer size too.
    if (model_) {
        model_->SetMaxAudioBufferSize(maxBlockSize);
    }
}

void NamStage::process(float* buffer, int numSamples)
{
    // Swap in a pending model if one has been set by the control thread.
    if (modelPending_.load(std::memory_order_acquire)) {
        applyPendingModel();
    }

    if (!model_) {
        // No model loaded -- pass through unmodified.
        return;
    }

    // Apply input calibration gain in-place before inference.
    if (inputGainLinear_ != 1.f) {
        for (int i = 0; i < numSamples; ++i) {
            buffer[i] *= inputGainLinear_;
        }
    }

    // Run inference. NeuralAudio writes to outputBuffer_.
    model_->Process(buffer, outputBuffer_.data(), numSamples);

    // Copy result back into the in-place buffer and apply output calibration.
    if (outputGainLinear_ != 1.f) {
        for (int i = 0; i < numSamples; ++i) {
            buffer[i] = outputBuffer_[i] * outputGainLinear_;
        }
    } else {
        std::memcpy(buffer, outputBuffer_.data(),
                    static_cast<std::size_t>(numSamples) * sizeof(float));
    }
}

void NamStage::reset()
{
    // NeuralAudio models maintain internal state (WaveNet receptive field,
    // LSTM hidden state). There is no public reset() on NeuralModel --
    // reloading the model clears state. For now, this is a no-op.
    // A silence-padding warmup can be added here in Phase 2 if needed.
}

bool NamStage::loadModel(const std::string& path)
{
    NeuralAudio::NeuralModel* raw = nullptr;

    try {
        raw = NeuralAudio::NeuralModel::CreateFromFile(path.c_str());
    } catch (...) {
        return false;
    }

    if (!raw) return false;

    auto newModel = std::unique_ptr<NeuralAudio::NeuralModel>(raw);

    if (maxBlockSize_ > 0) {
        newModel->SetMaxAudioBufferSize(maxBlockSize_);
    }

    // Stage the new model for swap at the top of the next process() call.
    pendingModel_    = std::move(newModel);
    pendingModelPath_ = path;
    modelPending_.store(true, std::memory_order_release);

    return true;
}

void NamStage::unloadModel()
{
    pendingModel_.reset();
    pendingModelPath_.clear();
    modelPending_.store(true, std::memory_order_release);
}

bool NamStage::hasModel() const
{
    return model_ != nullptr;
}

void NamStage::applyPendingModel()
{
    model_            = std::move(pendingModel_);
    currentModelPath_ = std::move(pendingModelPath_);
    modelPending_.store(false, std::memory_order_release);

    updateCalibration();
}

void NamStage::updateCalibration()
{
    if (!model_) {
        inputGainLinear_  = 1.f;
        outputGainLinear_ = 1.f;
        return;
    }

    const float inDb  = model_->GetRecommendedInputDBAdjustment();
    const float outDb = model_->GetRecommendedOutputDBAdjustment();

    inputGainLinear_  = std::pow(10.f, inDb  / 20.f);
    outputGainLinear_ = std::pow(10.f, outDb / 20.f);
}

} // namespace hexcaster
