#pragma once

#include "audio_engine.h"

#include <atomic>
#include <string>
#include <vector>

// Forward-declare ALSA types to avoid pulling alsa/asoundlib.h into consumer headers
struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace hexcaster {

/**
 * AlsaAudioEngine: direct ALSA PCM backend.
 *
 * Opens separate capture and playback handles (which may point to the same
 * hardware device or different ones). Runs a blocking read/write loop on the
 * calling thread with optional SCHED_FIFO real-time priority.
 *
 * Channel handling:
 *   - Capture: reads N-channel interleaved audio, extracts config.inputChannel
 *              to a mono float buffer.
 *   - Playback: takes mono float buffer, writes to channels selected by
 *               config.outputChannels bitmask. Unused channels get silence.
 *
 * Sample format negotiation:
 *   Probes for FLOAT_LE first, then S32_LE, then S16_LE. Conversion to/from
 *   float is handled internally -- the ProcessCallback always sees float.
 *
 * Xrun recovery:
 *   On EPIPE (underrun/overrun), calls snd_pcm_prepare() on the affected
 *   handle and continues. On unrecoverable errors, run() returns.
 */
class AlsaAudioEngine : public AudioEngine {
public:
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

private:
    enum class SampleFormat { Float32, Int32, Int16 };

    bool openHandle(const std::string& device, bool isCapture,
                    snd_pcm_t*& handle, unsigned int channels);
    bool negotiateFormat(snd_pcm_t* handle, unsigned int channels,
                         SampleFormat& fmt);
    bool recoverXrun(snd_pcm_t* handle, int err, const char* side);

    // Interleaved raw buffer -> mono float (extract one channel)
    void deinterleaveCapture(const void* raw, float* mono,
                              int frames, int totalChannels, int channel);

    // Mono float -> interleaved raw buffer (write selected channels)
    void interleavePlayback(const float* mono, void* raw,
                             int frames, int totalChannels, int channelMask);

    snd_pcm_t*    captureHandle_  = nullptr;
    snd_pcm_t*    playbackHandle_ = nullptr;

    Config        config_;
    SampleFormat  captureFmt_   = SampleFormat::Float32;
    SampleFormat  playbackFmt_  = SampleFormat::Float32;
    unsigned int  captureChannels_  = 2;
    unsigned int  playbackChannels_ = 2;
    unsigned int  actualRate_       = 0;
    unsigned int  actualFrames_     = 0;

    // Raw interleaved capture/playback buffers (allocated at open time)
    std::vector<uint8_t> captureRaw_;
    std::vector<uint8_t> playbackRaw_;

    // Mono float working buffer
    std::vector<float> monoBuffer_;

    ProcessCallback   callback_;
    std::atomic<bool> running_{ false };
    std::string       errorMsg_;
};

} // namespace hexcaster
