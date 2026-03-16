#include "alsa_audio_engine.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace hexcaster {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int AlsaAudioEngine::bytesPerSample(SampleFormat fmt)
{
    switch (fmt) {
        case SampleFormat::Float32: return 4;
        case SampleFormat::Int32:   return 4;
        case SampleFormat::Int16:   return 2;
    }
    return 2;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

AlsaAudioEngine::~AlsaAudioEngine()
{
    close();
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------

bool AlsaAudioEngine::open(const Config& config)
{
    config_ = config;
    captureChannels_  = 2;
    playbackChannels_ = 2;

    if (!openHandle(config_.inputDevice,  true,  captureHandle_,  captureChannels_,  captureFmt_))
        return false;

    if (!openHandle(config_.outputDevice, false, playbackHandle_, playbackChannels_, playbackFmt_))
        return false;

    // Pre-allocate raw interleaved buffers using actual negotiated format sizes
    const int frames = static_cast<int>(actualFrames_);
    captureRaw_.assign(
        static_cast<std::size_t>(frames) * captureChannels_ * bytesPerSample(captureFmt_), 0);
    playbackRaw_.assign(
        static_cast<std::size_t>(frames) * playbackChannels_ * bytesPerSample(playbackFmt_), 0);
    silenceRaw_.assign(playbackRaw_.size(), 0);
    monoBuffer_.assign(static_cast<std::size_t>(frames), 0.f);

    return true;
}

// ---------------------------------------------------------------------------
// openHandle()
//
// Key principle: all constraints are applied to the SAME hw_params object.
// We reset and re-constrain if a format attempt fails.
// ---------------------------------------------------------------------------

bool AlsaAudioEngine::openHandle(const std::string& device, bool isCapture,
                                   snd_pcm_t*& handle, unsigned int& channels,
                                   SampleFormat& fmt)
{
    const snd_pcm_stream_t stream = isCapture
        ? SND_PCM_STREAM_CAPTURE
        : SND_PCM_STREAM_PLAYBACK;

    int err = snd_pcm_open(&handle, device.c_str(), stream, 0);
    if (err < 0) {
        errorMsg_ = std::string("Failed to open ")
                  + (isCapture ? "capture" : "playback")
                  + " device '" + device + "': " + snd_strerror(err);
        return false;
    }

    // Format probe list -- S16_LE first as it has universal USB support.
    // The ProcessCallback always receives float; conversion happens at the edge.
    const struct { snd_pcm_format_t alsa; SampleFormat our; const char* name; } formats[] = {
        { SND_PCM_FORMAT_S16_LE,   SampleFormat::Int16,   "S16_LE"   },
        { SND_PCM_FORMAT_S32_LE,   SampleFormat::Int32,   "S32_LE"   },
        { SND_PCM_FORMAT_FLOAT_LE, SampleFormat::Float32, "FLOAT_LE" },
    };

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    bool configured = false;

    for (auto& f : formats) {
        // Start fresh each attempt
        snd_pcm_hw_params_any(handle, hw);

        if (snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
            continue;
        if (snd_pcm_hw_params_set_format(handle, hw, f.alsa) < 0)
            continue;

        // Rate
        unsigned int rate = config_.sampleRate;
        if (snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr) < 0)
            continue;
        actualRate_ = rate;

        // Channels -- try requested count, fall back to min/max
        if (snd_pcm_hw_params_set_channels(handle, hw, channels) < 0) {
            unsigned int minCh = 1, maxCh = 2;
            snd_pcm_hw_params_get_channels_min(hw, &minCh);
            snd_pcm_hw_params_get_channels_max(hw, &maxCh);
            channels = isCapture ? (unsigned int)(config_.inputChannel + 1) : 2u;
            channels = std::max(channels, minCh);
            channels = std::min(channels, maxCh);
            if (snd_pcm_hw_params_set_channels(handle, hw, channels) < 0)
                continue;
        }

        // Period size
        snd_pcm_uframes_t periodSize = config_.bufferFrames;
        if (snd_pcm_hw_params_set_period_size_near(handle, hw, &periodSize, nullptr) < 0)
            continue;

        // Number of periods (buffer = periods * period_size)
        unsigned int periods = config_.periods;
        snd_pcm_hw_params_set_periods_near(handle, hw, &periods, nullptr);

        // Commit
        err = snd_pcm_hw_params(handle, hw);
        if (err < 0) {
            std::fprintf(stderr, "hw_params commit failed for %s with %s: %s\n",
                         device.c_str(), f.name, snd_strerror(err));
            continue;
        }

        fmt = f.our;
        actualFrames_ = static_cast<unsigned int>(periodSize);
        configured = true;

        std::fprintf(stderr,
            "ALSA %s: device=%s format=%s channels=%u rate=%u period=%u\n",
            isCapture ? "capture " : "playback",
            device.c_str(), f.name, channels, actualRate_, actualFrames_);
        break;
    }

    if (!configured) {
        errorMsg_ = std::string("Could not configure ") +
                    (isCapture ? "capture" : "playback") +
                    " device '" + device + "' with any supported format";
        snd_pcm_close(handle);
        handle = nullptr;
        return false;
    }

    // SW params: start when the first period is written/read
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(handle, sw);
    snd_pcm_sw_params_set_start_threshold(handle, sw, actualFrames_);
    snd_pcm_sw_params_set_avail_min(handle, sw, actualFrames_);
    if (snd_pcm_sw_params(handle, sw) < 0) {
        std::fprintf(stderr, "Warning: sw_params failed for %s\n", device.c_str());
    }

    return true;
}

// ---------------------------------------------------------------------------
// setCallback()
// ---------------------------------------------------------------------------

void AlsaAudioEngine::setCallback(ProcessCallback cb)
{
    callback_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

void AlsaAudioEngine::run()
{
    if (!captureHandle_ || !playbackHandle_) {
        errorMsg_ = "run() called before open()";
        return;
    }
    if (!callback_) {
        errorMsg_ = "run() called without a process callback";
        return;
    }

    // Request SCHED_FIFO real-time priority (best-effort)
    {
        struct sched_param sp{};
        sp.sched_priority = 70;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr,
                "Warning: could not set RT priority (SCHED_FIFO). "
                "Run as root or add to 'audio' group with RT limits.\n");
        } else {
            std::fprintf(stderr, "RT priority set (SCHED_FIFO, priority 70).\n");
        }
    }

    // Streams are always on separate devices -- prepare both independently.
    snd_pcm_prepare(captureHandle_);
    snd_pcm_prepare(playbackHandle_);

    // Prime the playback buffer to prevent underrun before the first read
    primePlayback();

    running_.store(true, std::memory_order_release);

    const int frames = static_cast<int>(actualFrames_);

    std::fprintf(stderr,
        "Audio engine running: in=%s out=%s rate=%u frames=%d\n",
        config_.inputDevice.c_str(), config_.outputDevice.c_str(),
        actualRate_, frames);

    while (running_.load(std::memory_order_acquire)) {

        // --- Capture ---
        snd_pcm_sframes_t n = snd_pcm_readi(captureHandle_, captureRaw_.data(), frames);

        if (n < 0) {
            std::fprintf(stderr, "Capture error: %s -- recovering\n", snd_strerror(static_cast<int>(n)));
            if (!recoverBoth()) break;
            continue;
        }

        // Short read -- skip block, don't write garbage to output
        if (n != frames) continue;

        // --- Convert capture -> mono float ---
        deinterleaveCapture(captureRaw_.data(), monoBuffer_.data(),
                             frames, captureChannels_, config_.inputChannel);

        // --- DSP ---
        callback_(monoBuffer_.data(), frames);

        // --- Convert mono float -> playback ---
        interleavePlayback(monoBuffer_.data(), playbackRaw_.data(),
                            frames, playbackChannels_, config_.outputChannels);

        // --- Playback ---
        n = snd_pcm_writei(playbackHandle_, playbackRaw_.data(), frames);

        if (n < 0) {
            std::fprintf(stderr, "Playback error: %s -- recovering\n", snd_strerror(static_cast<int>(n)));
            if (!recoverBoth()) break;
            continue;
        }
    }

    std::fprintf(stderr, "Audio engine stopped.\n");
}

// ---------------------------------------------------------------------------
// stop() / close()
// ---------------------------------------------------------------------------

void AlsaAudioEngine::stop()
{
    running_.store(false, std::memory_order_release);
}

void AlsaAudioEngine::close()
{
    running_.store(false, std::memory_order_release);

    if (captureHandle_) {
        snd_pcm_drop(captureHandle_);
        snd_pcm_close(captureHandle_);
        captureHandle_ = nullptr;
    }
    if (playbackHandle_) {
        snd_pcm_drop(playbackHandle_);
        snd_pcm_close(playbackHandle_);
        playbackHandle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// recoverBoth()
// Recover both streams together to keep them in sync.
// Re-primes playback to avoid immediate re-underrun.
// ---------------------------------------------------------------------------

bool AlsaAudioEngine::recoverBoth()
{
    // Drop both streams unconditionally -- safe from any PCM state,
    // forces both handles back to SETUP.
    snd_pcm_drop(captureHandle_);
    snd_pcm_drop(playbackHandle_);

    // Prepare both independently (streams are always on separate devices).
    int err = snd_pcm_prepare(captureHandle_);
    if (err < 0) {
        std::fprintf(stderr, "Capture prepare failed: %s\n", snd_strerror(err));
        errorMsg_ = std::string("Capture prepare failed: ") + snd_strerror(err);
        return false;
    }

    err = snd_pcm_prepare(playbackHandle_);
    if (err < 0) {
        std::fprintf(stderr, "Playback prepare failed: %s\n", snd_strerror(err));
        errorMsg_ = std::string("Playback prepare failed: ") + snd_strerror(err);
        return false;
    }

    primePlayback();
    return true;
}

void AlsaAudioEngine::primePlayback()
{
    // Write `periods` blocks of silence to fill the playback buffer
    // before capture starts, so the output never starves on first block.
    for (unsigned int p = 0; p < config_.periods; ++p) {
        snd_pcm_writei(playbackHandle_, silenceRaw_.data(),
                       static_cast<snd_pcm_uframes_t>(actualFrames_));
    }
}

// ---------------------------------------------------------------------------
// Format conversion helpers
// ---------------------------------------------------------------------------

void AlsaAudioEngine::deinterleaveCapture(const void* raw, float* mono,
                                            int frames, int totalChannels,
                                            int channel)
{
    switch (captureFmt_) {
        case SampleFormat::Int16: {
            const int16_t* src = static_cast<const int16_t*>(raw);
            constexpr float kScale = 1.f / 32768.f;
            for (int i = 0; i < frames; ++i)
                mono[i] = static_cast<float>(src[i * totalChannels + channel]) * kScale;
            break;
        }
        case SampleFormat::Int32: {
            const int32_t* src = static_cast<const int32_t*>(raw);
            constexpr float kScale = 1.f / 2147483648.f;
            for (int i = 0; i < frames; ++i)
                mono[i] = static_cast<float>(src[i * totalChannels + channel]) * kScale;
            break;
        }
        case SampleFormat::Float32: {
            const float* src = static_cast<const float*>(raw);
            for (int i = 0; i < frames; ++i)
                mono[i] = src[i * totalChannels + channel];
            break;
        }
    }
}

void AlsaAudioEngine::interleavePlayback(const float* mono, void* raw,
                                          int frames, int totalChannels,
                                          int channelMask)
{
    switch (playbackFmt_) {
        case SampleFormat::Int16: {
            int16_t* dst = static_cast<int16_t*>(raw);
            std::memset(dst, 0,
                static_cast<std::size_t>(frames) * totalChannels * sizeof(int16_t));
            constexpr float kScale = 32767.f;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] =
                            static_cast<int16_t>(mono[i] * kScale);
            break;
        }
        case SampleFormat::Int32: {
            int32_t* dst = static_cast<int32_t*>(raw);
            std::memset(dst, 0,
                static_cast<std::size_t>(frames) * totalChannels * sizeof(int32_t));
            constexpr float kScale = 2147483647.f;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] =
                            static_cast<int32_t>(mono[i] * kScale);
            break;
        }
        case SampleFormat::Float32: {
            float* dst = static_cast<float*>(raw);
            std::memset(dst, 0,
                static_cast<std::size_t>(frames) * totalChannels * sizeof(float));
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] = mono[i];
            break;
        }
    }
}

} // namespace hexcaster
