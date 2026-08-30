#pragma once
#include "hexcaster/processor_stage.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace NeuralAudio { class NeuralModel; }
namespace hexcaster {

enum class NamQualityPolicy { Auto, Lite, Full };
enum class NamModelVariant { Unknown, A2Lite, A2Full, A2Composite, A2Custom };

struct NamModelInfo {
    std::string path;
    std::string version;
    NamModelVariant variant = NamModelVariant::Unknown;
    bool nativeStatic = false;
    bool qualityScalable = false;
    float selectedQuality = 1.f;
    float sampleRate = 0.f;
    int receptiveField = -1;
};

const char* namModelVariantName(NamModelVariant variant) noexcept;

class NamStage : public ProcessorStage {
public:
    NamStage();
    ~NamStage() override;
    NamStage(const NamStage&) = delete;
    NamStage& operator=(const NamStage&) = delete;

    void prepare(float sampleRate, int maxBlockSize) override;
    void process(float* buffer, int numSamples) override;
    void reset() override;

    // Construction, JSON parsing and prewarming occur here, off the RT thread.
    bool loadModel(const std::string& path,
                   NamQualityPolicy quality = NamQualityPolicy::Auto);
    void unloadModel();
    bool hasModel() const noexcept;
    std::string modelPath() const;
    NamModelInfo modelInfo() const;

private:
    struct PreparedModel;
    std::vector<std::unique_ptr<PreparedModel>> ownedModels_;
    std::atomic<PreparedModel*> activeModel_{nullptr};
    std::atomic<PreparedModel*> pendingModel_{nullptr};
    std::atomic<PreparedModel*> retiredModel_{nullptr};
    std::vector<float> outputBuffer_;
    int maxBlockSize_ = 0;
    float sampleRate_ = 0.f;

    void applyPendingModel() noexcept;
    void reclaimRetiredModels();
    static PreparedModel* unloadSentinel() noexcept;
    static PreparedModel* swapInProgressSentinel() noexcept;
};

} // namespace hexcaster
