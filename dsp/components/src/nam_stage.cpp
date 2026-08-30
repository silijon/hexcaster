#include "hexcaster/nam_stage.h"
#include "NeuralAudio/NeuralModel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace hexcaster {

struct NamStage::PreparedModel {
    std::unique_ptr<NeuralAudio::NeuralModel> model;
    NamModelInfo info;
    float inputGainLinear = 1.f;
    float outputGainLinear = 1.f;
};

namespace {
constexpr int kMaxInferenceFrames = 128; // Must match WAVENET_FRAMES in CMake.

bool isA2Version(const std::string& version)
{
    int major = 0, minor = 0, patch = 0;
    if (std::sscanf(version.c_str(), "%d.%d.%d", &major, &minor, &patch) != 3)
        return false;
    return major > 0 || minor > 5 || (minor == 5 && patch > 4);
}

NamModelVariant convertVariant(NeuralAudio::ENAMModelVariant variant)
{
    switch (variant) {
        case NeuralAudio::A2Lite: return NamModelVariant::A2Lite;
        case NeuralAudio::A2Full: return NamModelVariant::A2Full;
        case NeuralAudio::A2Composite: return NamModelVariant::A2Composite;
        default: return NamModelVariant::Unknown;
    }
}
} // namespace

const char* namModelVariantName(NamModelVariant variant) noexcept
{
    switch (variant) {
        case NamModelVariant::A2Lite: return "A2 Lite";
        case NamModelVariant::A2Full: return "A2 Full";
        case NamModelVariant::A2Composite: return "A2 Composite";
        case NamModelVariant::A2Custom: return "A2 Custom (NAMCore)";
        default: return "Unknown";
    }
}

NamStage::NamStage() = default;
NamStage::~NamStage() = default;

NamStage::PreparedModel* NamStage::unloadSentinel() noexcept
{
    return reinterpret_cast<PreparedModel*>(1);
}

NamStage::PreparedModel* NamStage::swapInProgressSentinel() noexcept
{
    return reinterpret_cast<PreparedModel*>(2);
}

void NamStage::prepare(float sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;
    outputBuffer_.assign(static_cast<std::size_t>(maxBlockSize), 0.f);
    for (auto& prepared : ownedModels_)
        prepared->model->SetMaxAudioBufferSize(std::min(maxBlockSize, kMaxInferenceFrames));
}

void NamStage::process(float* buffer, int numSamples)
{
    applyPendingModel();
    auto* prepared = activeModel_.load(std::memory_order_acquire);
    if (!prepared || numSamples <= 0 || numSamples > maxBlockSize_)
        return;
    if (prepared->inputGainLinear != 1.f)
        for (int i = 0; i < numSamples; ++i) buffer[i] *= prepared->inputGainLinear;

    for (int offset = 0; offset < numSamples; offset += kMaxInferenceFrames) {
        const int frames = std::min(kMaxInferenceFrames, numSamples - offset);
        prepared->model->Process(buffer + offset, outputBuffer_.data() + offset,
                                 static_cast<std::size_t>(frames));
    }
    if (prepared->outputGainLinear != 1.f) {
        for (int i = 0; i < numSamples; ++i)
            buffer[i] = outputBuffer_[i] * prepared->outputGainLinear;
    } else {
        std::memcpy(buffer, outputBuffer_.data(), static_cast<std::size_t>(numSamples) * sizeof(float));
    }
}

void NamStage::reset() {}

bool NamStage::loadModel(const std::string& path, NamQualityPolicy quality)
{
    reclaimRetiredModels();
    if (pendingModel_.load(std::memory_order_acquire)) return false;

    NeuralAudio::NeuralModelLoader loader;
    loader.SetAudioInputLevelDBu(interfaceInputLevelDBu_);
    loader.SetDefaultMaxAudioBufferSize(
        std::min(maxBlockSize_ > 0 ? maxBlockSize_ : kMaxInferenceFrames,
                 kMaxInferenceFrames));
    loader.SetExternalSampleRate(static_cast<int>(sampleRate_ > 0.f ? sampleRate_ : 48000.f));
    loader.SetWaveNetLoadMode(NeuralAudio::Internal);
    loader.SetCompositeModelLoadMode(NeuralAudio::LoadAll);
    loader.SetDefaultQualityScaleFactor(quality == NamQualityPolicy::Full ? 1.f : 0.f);

    NeuralAudio::NeuralModel* raw = nullptr;
    try { raw = loader.CreateFromFile(path, true); } catch (...) { return false; }
    if (!raw) return false;

    auto prepared = std::make_unique<PreparedModel>();
    prepared->model.reset(raw);
    prepared->info.path = path;
    prepared->info.version = raw->GetModelVersion();
    if (!isA2Version(prepared->info.version)) return false;
    prepared->info.variant = convertVariant(raw->GetNAMModelVariant());
    if (prepared->info.variant == NamModelVariant::Unknown)
        prepared->info.variant = NamModelVariant::A2Custom;
    prepared->info.nativeStatic = raw->IsStatic();
    prepared->info.qualityScalable = raw->HasQualityScaling();
    prepared->info.selectedQuality = raw->GetQualityScaleFactor();
    prepared->info.sampleRate = raw->GetSampleRate();
    prepared->info.receptiveField = raw->GetReceptiveFieldSize();
    prepared->info.interfaceInputLevelDBu = interfaceInputLevelDBu_;

    // NeuralAudio uses 12 dBu internally when input_level_dbu is absent. Do
    // not mistake that library default for known capture calibration.
    const std::string modelInputMetadata = raw->GetMetadata("input_level_dbu");
    if (!modelInputMetadata.empty()) {
        char* end = nullptr;
        const float modelLevel = std::strtof(modelInputMetadata.c_str(), &end);
        if (end != modelInputMetadata.c_str() && *end == '\0' && std::isfinite(modelLevel)) {
            prepared->info.hasInputCalibrationMetadata = true;
            prepared->info.modelInputLevelDBu = modelLevel;
            prepared->info.inputCalibrationDb = raw->GetRecommendedInputDBAdjustment();
        }
    }

    if (quality == NamQualityPolicy::Lite && prepared->info.variant == NamModelVariant::A2Full) return false;
    if (quality == NamQualityPolicy::Full && prepared->info.variant == NamModelVariant::A2Lite) return false;

    prepared->inputGainLinear = std::pow(10.f, prepared->info.inputCalibrationDb / 20.f);
    prepared->outputGainLinear = std::pow(10.f, raw->GetRecommendedOutputDBAdjustment() / 20.f);
    auto* published = prepared.get();
    ownedModels_.push_back(std::move(prepared));
    pendingModel_.store(published, std::memory_order_release);
    return true;
}

void NamStage::setInterfaceInputLevelDBu(float levelDBu) noexcept
{
    if (std::isfinite(levelDBu)) interfaceInputLevelDBu_ = levelDBu;
}

void NamStage::unloadModel()
{
    reclaimRetiredModels();
    if (!pendingModel_.load(std::memory_order_acquire))
        pendingModel_.store(unloadSentinel(), std::memory_order_release);
}

bool NamStage::hasModel() const noexcept
{
    auto* pending = pendingModel_.load(std::memory_order_acquire);
    return activeModel_.load(std::memory_order_acquire) ||
           (pending && pending != unloadSentinel() && pending != swapInProgressSentinel());
}

std::string NamStage::modelPath() const { return modelInfo().path; }

NamModelInfo NamStage::modelInfo() const
{
    auto* prepared = activeModel_.load(std::memory_order_acquire);
    if (!prepared) prepared = pendingModel_.load(std::memory_order_acquire);
    if (prepared == swapInProgressSentinel())
        prepared = activeModel_.load(std::memory_order_acquire);
    return prepared && prepared != unloadSentinel() ? prepared->info : NamModelInfo{};
}

void NamStage::applyPendingModel() noexcept
{
    auto* next = pendingModel_.load(std::memory_order_acquire);
    if (!next) return;
    if (!pendingModel_.compare_exchange_strong(next, swapInProgressSentinel(),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
        return;
    if (next == unloadSentinel()) next = nullptr;
    auto* previous = activeModel_.exchange(next, std::memory_order_acq_rel);
    retiredModel_.store(previous, std::memory_order_release);
    pendingModel_.store(nullptr, std::memory_order_release);
}

void NamStage::reclaimRetiredModels()
{
    auto* retired = retiredModel_.exchange(nullptr, std::memory_order_acq_rel);
    if (!retired) return;
    ownedModels_.erase(std::remove_if(ownedModels_.begin(), ownedModels_.end(),
        [retired](const auto& model) { return model.get() == retired; }), ownedModels_.end());
}

} // namespace hexcaster
