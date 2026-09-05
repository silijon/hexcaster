#pragma once

#include "audio_engine.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Forward-declare ALSA types to avoid pulling alsa/asoundlib.h into consumer headers
struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace hexcaster {

/**
 * AlsaAudioEngine: direct ALSA PCM backend.
 *
 * Opens separate capture and playback handles on independent devices.
 * HexCaster always uses separate ADC (USB input) and DAC (amp board output)
 * devices, so snd_pcm_link() is not used.
 * run() occupies the calling thread, but PCM reads and writes are nonblocking
 * and coordinated with bounded snd_pcm_wait() calls. stop() only signals the
 * loop; all PCM-handle operations remain on the realtime thread to avoid
 * cross-thread ALSA/driver deadlocks. The thread requests SCHED_FIFO priority.
 *
 * Channel handling:
 *   - Capture: reads N-channel interleaved audio, extracts config.inputChannel
 *              to a mono float buffer.
 *   - Playback: takes mono float buffer, writes to channels selected by
 *               config.outputChannels bitmask. Unused channels get silence.
 *
 * Sample format negotiation:
 *   Probes for S16_LE first (universal USB support), then S32_LE, then
 *   FLOAT_LE. Conversion to/from float is handled internally -- the
 *   ProcessCallback always sees float.
 *
 * Xrun recovery:
 *   On EPIPE (underrun/overrun), prepares both handles and re-primes the
 *   playback buffer before resuming to prevent cascade xruns.
 */
class AlsaAudioEngine : public AudioEngine {
public:
    struct IoStatus {
        uint64_t errors           = 0;
        uint64_t recoveryAttempts = 0;
        uint64_t recoveryFailures = 0;
        int      lastErrorCode    = 0;
        bool     lastWasCapture   = false;
    };

    AlsaAudioEngine() = default;
    ~AlsaAudioEngine() override;

    AlsaAudioEngine(const AlsaAudioEngine&)            = delete;
    AlsaAudioEngine& operator=(const AlsaAudioEngine&) = delete;

    bool open(const Config& config) override;
    void setCallback(ProcessCallback cb) override;
    void run() override;
    void stop() override;
    void close() override;

    const std::string& errorMessage() const override { return errorMsg_; }
    unsigned int actualSampleRate()   const override { return actualRate_; }
    unsigned int actualBufferFrames() const override { return actualFrames_; }
    RealtimeMetrics realtimeMetrics() const override;
    IoStatus ioStatus() const noexcept;

private:
    enum class SampleFormat { 
        Int24Packed, 
        Float32, 
        Int32, 
        Int16 
    };

    static int bytesPerSample(SampleFormat fmt);

    struct DeviceSettings {
        unsigned int sampleRate   = 0;
        unsigned int periodFrames = 0;
        unsigned int bufferFrames = 0;
        unsigned int periods      = 0;
    };

    static constexpr uint64_t kMetricBinNs = 1000;   // 1 us bins
    static constexpr std::size_t kMetricBins = 10001; // covers 0-10 ms

    struct MetricAccumulator {
        uint64_t blockCount       = 0;
        uint64_t processingTotalNs = 0;
        uint64_t processingMaxNs   = 0;
        uint64_t callbackTotalNs   = 0;
        uint64_t callbackMaxNs     = 0;
        uint64_t deadlineMisses    = 0;
        uint64_t captureOverruns   = 0;
        uint64_t playbackUnderruns = 0;
        uint64_t shortReads        = 0;
        uint64_t shortWrites       = 0;
        uint64_t recoveryAttempts  = 0;
        uint64_t recoveryFailures  = 0;
        std::array<uint32_t, kMetricBins> processingHistogram{};
        std::array<uint32_t, kMetricBins> callbackHistogram{};
    };

    bool openHandle(const std::string& device, bool isCapture,
                    snd_pcm_t*& handle, unsigned int& channels,
                    SampleFormat& fmt, DeviceSettings& settings);

    bool recoverBoth();
    bool startCaptureAndPrimePlayback();
    bool readCaptureBlock(int frames);
    bool writePlaybackBlock(const void* data, int frames);
    bool waitForIo(snd_pcm_t* handle, bool capture, int& waitAttempts);

    bool primePlayback();
    void recordDuration(std::array<uint32_t, kMetricBins>& histogram,
                        uint64_t durationNs, uint64_t& totalNs,
                        uint64_t& maxNs);
    void recordIoError(bool capture, int errorCode);
    static uint64_t monotonicRawNs() noexcept;
    static uint64_t percentileNs(const std::array<uint32_t, kMetricBins>& histogram,
                                 uint64_t count, unsigned int perThousand) noexcept;

    // Interleaved raw buffer -> mono float (extract one channel)
    void deinterleaveCapture(const void* raw, float* mono,
                              int frames, int totalChannels, int channel);

    // Mono float -> interleaved raw buffer (write selected channels)
    void interleavePlayback(const float* mono, void* raw,
                             int frames, int totalChannels, int channelMask);

    snd_pcm_t*    captureHandle_  = nullptr;
    snd_pcm_t*    playbackHandle_ = nullptr;

    Config        config_;
    SampleFormat  captureFmt_       = SampleFormat::Int16;
    SampleFormat  playbackFmt_      = SampleFormat::Int16;
    unsigned int  captureChannels_  = 2;
    unsigned int  playbackChannels_ = 2;
    unsigned int  actualRate_       = 0;
    unsigned int  actualFrames_     = 0;
    DeviceSettings captureSettings_;
    DeviceSettings playbackSettings_;

    // Raw interleaved capture/playback buffers (allocated at open time)
    std::vector<uint8_t> captureRaw_;
    std::vector<uint8_t> playbackRaw_;

    // Silence buffer for playback priming (same size as playbackRaw_)
    std::vector<uint8_t> silenceRaw_;

    // Mono float working buffer
    std::vector<float> monoBuffer_;

    ProcessCallback   callback_;
    std::atomic<bool> running_{ false };
    std::string       errorMsg_;
    int               lastAlsaError_ = 0;
    bool              lastErrorWasCapture_ = false;
    int               ioWaitTimeoutMs_ = 20;
    std::atomic<uint64_t> ioErrorCount_{ 0 };
    std::atomic<uint64_t> liveRecoveryAttempts_{ 0 };
    std::atomic<uint64_t> liveRecoveryFailures_{ 0 };
    std::atomic<int>      liveLastErrorCode_{ 0 };
    std::atomic<bool>     liveLastWasCapture_{ false };
    MetricAccumulator metrics_;
};

} // namespace hexcaster
