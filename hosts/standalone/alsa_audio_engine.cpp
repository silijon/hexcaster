#include "alsa_audio_engine.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace hexcaster {

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

    // Determine hardware channel counts. Most USB interfaces are stereo (2).
    // We always open the device at its native channel count and extract/inject
    // the channels we need at the conversion boundary.
    captureChannels_  = 2;
    playbackChannels_ = 2;

    if (!openHandle(config_.inputDevice, true, captureHandle_, captureChannels_))
        return false;

    if (!openHandle(config_.outputDevice, false, playbackHandle_, playbackChannels_))
        return false;

    // Pre-allocate raw interleaved buffers.
    // Worst case: Int32 = 4 bytes per sample.
    const int frames  = static_cast<int>(actualFrames_);
    captureRaw_.resize(static_cast<std::size_t>(frames) * captureChannels_  * 4, 0);
    playbackRaw_.resize(static_cast<std::size_t>(frames) * playbackChannels_ * 4, 0);
    monoBuffer_.resize(static_cast<std::size_t>(frames), 0.f);

    return true;
}

bool AlsaAudioEngine::openHandle(const std::string& device, bool isCapture,
                                   snd_pcm_t*& handle, unsigned int channels)
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

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle, hw);

    // Access: interleaved r/w
    err = snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        errorMsg_ = std::string("Cannot set interleaved access: ") + snd_strerror(err);
        return false;
    }

    // Format: probe best available
    SampleFormat& fmt = isCapture ? captureFmt_ : playbackFmt_;
    if (!negotiateFormat(handle, channels, fmt))
        return false;

    // Sample rate
    unsigned int rate = config_.sampleRate;
    err = snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr);
    if (err < 0) {
        errorMsg_ = std::string("Cannot set sample rate: ") + snd_strerror(err);
        return false;
    }
    actualRate_ = rate;

    // Channels
    err = snd_pcm_hw_params_set_channels(handle, hw, channels);
    if (err < 0) {
        // Some devices only expose one channel count -- try what's available
        unsigned int minCh = 0, maxCh = 0;
        snd_pcm_hw_params_get_channels_min(hw, &minCh);
        snd_pcm_hw_params_get_channels_max(hw, &maxCh);
        channels = (isCapture ? config_.inputChannel + 1 : 2);
        channels = std::max(channels, minCh);
        channels = std::min(channels, maxCh);
        err = snd_pcm_hw_params_set_channels(handle, hw, channels);
        if (err < 0) {
            errorMsg_ = std::string("Cannot set channels: ") + snd_strerror(err);
            return false;
        }
    }
    if (isCapture)  captureChannels_  = channels;
    else            playbackChannels_ = channels;

    // Buffer/period size
    snd_pcm_uframes_t periodSize = config_.bufferFrames;
    err = snd_pcm_hw_params_set_period_size_near(handle, hw, &periodSize, nullptr);
    if (err < 0) {
        errorMsg_ = std::string("Cannot set period size: ") + snd_strerror(err);
        return false;
    }
    actualFrames_ = static_cast<unsigned int>(periodSize);

    unsigned int periods = config_.periods;
    err = snd_pcm_hw_params_set_periods_near(handle, hw, &periods, nullptr);
    if (err < 0) {
        errorMsg_ = std::string("Cannot set periods: ") + snd_strerror(err);
        return false;
    }

    // Commit hw params
    err = snd_pcm_hw_params(handle, hw);
    if (err < 0) {
        errorMsg_ = std::string("Cannot apply hw params: ") + snd_strerror(err);
        return false;
    }

    // SW params: auto-start on first read/write
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(handle, sw);
    snd_pcm_sw_params_set_start_threshold(handle, sw, periodSize);
    snd_pcm_sw_params_set_avail_min(handle, sw, periodSize);
    snd_pcm_sw_params(handle, sw);

    return true;
}

bool AlsaAudioEngine::negotiateFormat(snd_pcm_t* handle, unsigned int /*channels*/,
                                       SampleFormat& fmt)
{
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle, hw);

    // Probe in preference order: native float, then 32-bit int, then 16-bit int
    const struct { snd_pcm_format_t alsa; SampleFormat fmt; } candidates[] = {
        { SND_PCM_FORMAT_FLOAT_LE, SampleFormat::Float32 },
        { SND_PCM_FORMAT_S32_LE,   SampleFormat::Int32   },
        { SND_PCM_FORMAT_S16_LE,   SampleFormat::Int16   },
    };

    for (auto& c : candidates) {
        if (snd_pcm_hw_params_test_format(handle, hw, c.alsa) == 0) {
            int err = snd_pcm_hw_params_set_format(handle, hw, c.alsa);
            if (err == 0) {
                fmt = c.fmt;
                return true;
            }
        }
    }

    errorMsg_ = "No supported sample format found (tried FLOAT_LE, S32_LE, S16_LE)";
    return false;
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

    // Request SCHED_FIFO real-time priority. Best-effort: continue if denied.
    {
        struct sched_param sp{};
        sp.sched_priority = 70;  // below kernel IRQ handlers (~90), above normal
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr,
                "Warning: could not set RT priority (SCHED_FIFO). "
                "Run as root or configure /etc/security/limits.conf.\n");
        } else {
            std::fprintf(stderr, "RT priority set (SCHED_FIFO, priority 70).\n");
        }
    }

    // Prepare both handles
    snd_pcm_prepare(captureHandle_);
    snd_pcm_prepare(playbackHandle_);

    // Prime the playback buffer with silence to avoid underrun on first read
    {
        const int frames = static_cast<int>(actualFrames_);
        std::memset(playbackRaw_.data(), 0, playbackRaw_.size());
        for (unsigned int p = 0; p < config_.periods; ++p) {
            snd_pcm_writei(playbackHandle_, playbackRaw_.data(), frames);
        }
    }

    running_.store(true, std::memory_order_release);

    const int frames = static_cast<int>(actualFrames_);

    std::fprintf(stderr,
        "Audio engine running: in=%s out=%s rate=%u frames=%d\n",
        config_.inputDevice.c_str(), config_.outputDevice.c_str(),
        actualRate_, frames);

    while (running_.load(std::memory_order_acquire)) {

        // --- Capture ---
        snd_pcm_sframes_t n = snd_pcm_readi(
            captureHandle_, captureRaw_.data(), frames);

        if (n == -EPIPE || n == -ESTRPIPE) {
            if (!recoverXrun(captureHandle_, static_cast<int>(n), "capture"))
                break;
            continue;
        }
        if (n < 0) {
            errorMsg_ = std::string("Capture error: ") + snd_strerror(static_cast<int>(n));
            break;
        }
        if (n != frames) continue; // short read -- skip this block

        // --- Convert capture: interleaved raw -> mono float ---
        deinterleaveCapture(captureRaw_.data(), monoBuffer_.data(),
                             frames, captureChannels_, config_.inputChannel);

        // --- Process ---
        callback_(monoBuffer_.data(), frames);

        // --- Convert playback: mono float -> interleaved raw ---
        interleavePlayback(monoBuffer_.data(), playbackRaw_.data(),
                            frames, playbackChannels_, config_.outputChannels);

        // --- Playback ---
        n = snd_pcm_writei(playbackHandle_, playbackRaw_.data(), frames);

        if (n == -EPIPE || n == -ESTRPIPE) {
            if (!recoverXrun(playbackHandle_, static_cast<int>(n), "playback"))
                break;
            continue;
        }
        if (n < 0) {
            errorMsg_ = std::string("Playback error: ") + snd_strerror(static_cast<int>(n));
            break;
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
// Xrun recovery
// ---------------------------------------------------------------------------

bool AlsaAudioEngine::recoverXrun(snd_pcm_t* handle, int err, const char* side)
{
    std::fprintf(stderr, "ALSA xrun on %s (%s) -- recovering\n",
                 side, snd_strerror(err));

    if (err == -EPIPE) {
        int r = snd_pcm_prepare(handle);
        if (r < 0) {
            errorMsg_ = std::string("Xrun recovery failed on ")
                      + side + ": " + snd_strerror(r);
            return false;
        }
        return true;
    }
    if (err == -ESTRPIPE) {
        // Suspended -- wait for resume
        int r;
        while ((r = snd_pcm_resume(handle)) == -EAGAIN)
            usleep(1000);
        if (r < 0) snd_pcm_prepare(handle);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Format conversion helpers
// ---------------------------------------------------------------------------

void AlsaAudioEngine::deinterleaveCapture(const void* raw, float* mono,
                                            int frames, int totalChannels,
                                            int channel)
{
    switch (captureFmt_) {
        case SampleFormat::Float32: {
            const float* src = static_cast<const float*>(raw);
            for (int i = 0; i < frames; ++i)
                mono[i] = src[i * totalChannels + channel];
            break;
        }
        case SampleFormat::Int32: {
            const int32_t* src = static_cast<const int32_t*>(raw);
            constexpr float kScale = 1.f / 2147483648.f;
            for (int i = 0; i < frames; ++i)
                mono[i] = static_cast<float>(src[i * totalChannels + channel]) * kScale;
            break;
        }
        case SampleFormat::Int16: {
            const int16_t* src = static_cast<const int16_t*>(raw);
            constexpr float kScale = 1.f / 32768.f;
            for (int i = 0; i < frames; ++i)
                mono[i] = static_cast<float>(src[i * totalChannels + channel]) * kScale;
            break;
        }
    }
}

void AlsaAudioEngine::interleavePlayback(const float* mono, void* raw,
                                          int frames, int totalChannels,
                                          int channelMask)
{
    switch (playbackFmt_) {
        case SampleFormat::Float32: {
            float* dst = static_cast<float*>(raw);
            std::memset(dst, 0, static_cast<std::size_t>(frames) * totalChannels * sizeof(float));
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] = mono[i];
            break;
        }
        case SampleFormat::Int32: {
            int32_t* dst = static_cast<int32_t*>(raw);
            std::memset(dst, 0, static_cast<std::size_t>(frames) * totalChannels * sizeof(int32_t));
            constexpr float kScale = 2147483647.f;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] = static_cast<int32_t>(mono[i] * kScale);
            break;
        }
        case SampleFormat::Int16: {
            int16_t* dst = static_cast<int16_t*>(raw);
            std::memset(dst, 0, static_cast<std::size_t>(frames) * totalChannels * sizeof(int16_t));
            constexpr float kScale = 32767.f;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] = static_cast<int16_t>(mono[i] * kScale);
            break;
        }
    }
}

} // namespace hexcaster
