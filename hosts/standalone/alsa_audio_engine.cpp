#include "alsa_audio_engine.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace hexcaster {

namespace {

void enableFlushToZero() noexcept
{
#if defined(__aarch64__)
    // AArch64 FPCR bit 24 is FZ (flush subnormal results to zero).
    uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (uint64_t{1} << 24);
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
}

void prefaultAudioStack() noexcept
{
    // Touch enough stack for the engine loop, callback dispatch, and DSP call
    // chain before entering the blocking realtime loop. mlockall(MCL_FUTURE)
    // keeps these pages resident when it succeeds.
    constexpr std::size_t kPrefaultBytes = 64 * 1024;
    constexpr std::size_t kPageBytes = 4096;
    volatile char stack[kPrefaultBytes];
    for (std::size_t offset = 0; offset < kPrefaultBytes; offset += kPageBytes)
        stack[offset] = 0;
    (void)stack[0];
}

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int AlsaAudioEngine::bytesPerSample(SampleFormat fmt)
{
    switch (fmt) {
        case SampleFormat::Float32:         return 4;
        case SampleFormat::Int32:           return 4;
        case SampleFormat::Int24Packed:     return 3;
        case SampleFormat::Int16:           return 2;
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
    if (config.sampleRate == 0 || config.bufferFrames == 0 || config.periods < 2) {
        errorMsg_ = "Invalid ALSA configuration: sample rate and period must be non-zero; at least two periods are required";
        return false;
    }

    config_ = config;
    captureChannels_  = 1;
    playbackChannels_ = 2;
    captureSettings_ = {};
    playbackSettings_ = {};
    actualRate_ = 0;
    actualFrames_ = 0;
    lastAlsaError_ = 0;
    metrics_ = {};
    ioErrorCount_.store(0, std::memory_order_relaxed);
    liveRecoveryAttempts_.store(0, std::memory_order_relaxed);
    liveRecoveryFailures_.store(0, std::memory_order_relaxed);
    liveLastErrorCode_.store(0, std::memory_order_relaxed);
    liveLastWasCapture_.store(false, std::memory_order_relaxed);

    if (!openHandle(config_.inputDevice, true, captureHandle_, captureChannels_,
                    captureFmt_, captureSettings_))
        return false;

    if (!openHandle(config_.outputDevice, false, playbackHandle_, playbackChannels_,
                    playbackFmt_, playbackSettings_)) {
        close();
        return false;
    }

    // The synchronous read -> process -> write loop requires equal sample
    // rates and period sizes. Do not silently use the output device's values
    // for capture buffers when the devices negotiated differently.
    if (captureSettings_.sampleRate != playbackSettings_.sampleRate ||
        captureSettings_.periodFrames != playbackSettings_.periodFrames) {
        errorMsg_ = "Capture/playback ALSA settings differ: capture rate="
            + std::to_string(captureSettings_.sampleRate)
            + " period=" + std::to_string(captureSettings_.periodFrames)
            + ", playback rate=" + std::to_string(playbackSettings_.sampleRate)
            + " period=" + std::to_string(playbackSettings_.periodFrames)
            + ". This synchronous backend requires matching rate and period size.";
        close();
        return false;
    }

    actualRate_ = captureSettings_.sampleRate;
    actualFrames_ = captureSettings_.periodFrames;
    // Waiting longer than several periods cannot preserve realtime operation;
    // use a floor for startup/scheduler jitter while keeping all I/O bounded.
    const uint64_t fourPeriodsMs =
        (static_cast<uint64_t>(actualFrames_) * 4000ull + actualRate_ - 1)
        / actualRate_;
    ioWaitTimeoutMs_ = static_cast<int>(std::max<uint64_t>(10, fourPeriodsMs));

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
                                   SampleFormat& fmt, DeviceSettings& settings)
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
        { SND_PCM_FORMAT_S24_3LE,  SampleFormat::Int24Packed,   "S24_3LE"  },
        { SND_PCM_FORMAT_S16_LE,   SampleFormat::Int16,         "S16_LE"   },
        { SND_PCM_FORMAT_S32_LE,   SampleFormat::Int32,         "S32_LE"   },
        { SND_PCM_FORMAT_FLOAT_LE, SampleFormat::Float32,       "FLOAT_LE" },
    };

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    bool configured = false;

    for (auto& f : formats) {
        // Start fresh each attempt
        snd_pcm_hw_params_any(handle, hw);

        if (!isCapture && f.alsa == SND_PCM_FORMAT_S24_3LE)
            continue; // Don't accept 3-bit format on playback
        if (snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
            continue;
        if (snd_pcm_hw_params_set_format(handle, hw, f.alsa) < 0)
            continue;

        // Rate
        unsigned int rate = config_.sampleRate;
        if (snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr) < 0)
            continue;
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

        // Query the committed parameters. ALSA's *_near calls may choose
        // values different from the requested values.
        snd_pcm_hw_params_current(handle, hw);
        snd_pcm_uframes_t actualPeriod = 0;
        snd_pcm_uframes_t actualBuffer = 0;
        unsigned int actualRate = 0;
        unsigned int actualPeriods = 0;
        snd_pcm_hw_params_get_rate(hw, &actualRate, nullptr);
        snd_pcm_hw_params_get_period_size(hw, &actualPeriod, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw, &actualBuffer);
        snd_pcm_hw_params_get_periods(hw, &actualPeriods, nullptr);

        fmt = f.our;
        settings.sampleRate = actualRate;
        settings.periodFrames = static_cast<unsigned int>(actualPeriod);
        settings.bufferFrames = static_cast<unsigned int>(actualBuffer);
        settings.periods = actualPeriods;
        configured = true;

        std::fprintf(stderr,
            "ALSA %s: device=%s format=%s channels=%u rate=%u period=%u buffer=%u periods=%u\n",
            isCapture ? "capture " : "playback",
            device.c_str(), f.name, channels, settings.sampleRate,
            settings.periodFrames, settings.bufferFrames, settings.periods);
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

    // Playback starts only once its complete ALSA buffer has been primed.
    // Capture starts naturally on the first read, after playback is ready;
    // starting capture before priming can overrun a two-period USB buffer.
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(handle, sw);
    snd_pcm_sw_params_set_start_threshold(
        handle, sw, isCapture ? settings.periodFrames : settings.bufferFrames);
    snd_pcm_sw_params_set_avail_min(handle, sw, settings.periodFrames);
    if (snd_pcm_sw_params(handle, sw) < 0) {
        std::fprintf(stderr, "Warning: sw_params failed for %s\n", device.c_str());
    }

    // All steady-state I/O is driven by bounded snd_pcm_wait() calls. ALSA's
    // read/write functions themselves must never be able to block forever.
    if (snd_pcm_nonblock(handle, 1) < 0) {
        errorMsg_ = std::string("Could not enable nonblocking ALSA I/O for '")
                  + device + "'";
        snd_pcm_close(handle);
        handle = nullptr;
        return false;
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

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr,
            "Warning: mlockall failed: %s. Realtime page-fault protection is unavailable.\n",
            std::strerror(errno));
    }
    prefaultAudioStack();
    enableFlushToZero();

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
    int err = snd_pcm_prepare(captureHandle_);
    if (err < 0) {
        errorMsg_ = std::string("Capture prepare failed: ") + snd_strerror(err);
        return;
    }
    err = snd_pcm_prepare(playbackHandle_);
    if (err < 0) {
        errorMsg_ = std::string("Playback prepare failed: ") + snd_strerror(err);
        return;
    }

    // Mark the loop stoppable before priming: nonblocking priming may wait for
    // playback readiness, and stop() must be able to interrupt that wait.
    running_.store(true, std::memory_order_release);

    // Prime playback before the first capture read starts the USB stream. This
    // keeps capture stopped while ALSA initializes and fills playback.
    if (!primePlayback()) {
        running_.store(false, std::memory_order_release);
        errorMsg_ = std::string("Playback priming failed: ")
                  + snd_strerror(lastAlsaError_);
        return;
    }

    const int frames = static_cast<int>(actualFrames_);

    std::fprintf(stderr,
        "Audio engine running: in=%s out=%s rate=%u frames=%d\n",
        config_.inputDevice.c_str(), config_.outputDevice.c_str(),
        actualRate_, frames);

    while (running_.load(std::memory_order_acquire)) {

        // --- Capture ---
        if (!readCaptureBlock(frames)) {
            if (!running_.load(std::memory_order_acquire))
                break;
            if (!recoverBoth()) break;
            continue;
        }

        const bool measure = config_.enableRealtimeMetrics;
        const uint64_t processingStartNs = measure ? monotonicRawNs() : 0;

        // --- Convert capture -> mono float ---
        deinterleaveCapture(captureRaw_.data(), monoBuffer_.data(),
                             frames, captureChannels_, config_.inputChannel);

        // --- DSP ---
        const uint64_t callbackStartNs = measure ? monotonicRawNs() : 0;
        callback_(monoBuffer_.data(), frames);
        if (measure) {
            recordDuration(metrics_.callbackHistogram,
                           monotonicRawNs() - callbackStartNs,
                           metrics_.callbackTotalNs, metrics_.callbackMaxNs);
        }

        // --- Convert mono float -> playback ---
        interleavePlayback(monoBuffer_.data(), playbackRaw_.data(),
                            frames, playbackChannels_, config_.outputChannels);

        if (measure) {
            const uint64_t processingNs = monotonicRawNs() - processingStartNs;
            recordDuration(metrics_.processingHistogram, processingNs,
                           metrics_.processingTotalNs, metrics_.processingMaxNs);
            ++metrics_.blockCount;
            const uint64_t deadlineNs =
                (static_cast<uint64_t>(frames) * 1000000000ull) / actualRate_;
            if (processingNs > deadlineNs)
                ++metrics_.deadlineMisses;
        }

        // --- Playback ---
        if (!writePlaybackBlock(playbackRaw_.data(), frames)) {
            if (!running_.load(std::memory_order_acquire))
                break;
            if (!recoverBoth()) break;
            continue;
        }
    }

    if (lastAlsaError_ < 0 && errorMsg_.empty()) {
        errorMsg_ = std::string(lastErrorWasCapture_ ? "Capture" : "Playback")
                  + " error: " + snd_strerror(lastAlsaError_);
    }

    std::fprintf(stderr, "Audio engine stopped.\n");
}

// ---------------------------------------------------------------------------
// stop() / close()
// ---------------------------------------------------------------------------

void AlsaAudioEngine::stop()
{
    // Do not call ALSA from this control/watcher thread. Concurrent access to
    // a PCM handle while the realtime thread is in wait/read/write can wedge
    // inside the ALSA driver and become unkillable. PCM I/O is nonblocking and
    // snd_pcm_wait() has a finite timeout, so clearing this flag is sufficient;
    // run() will unwind within one bounded wait interval.
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
    liveRecoveryAttempts_.fetch_add(1, std::memory_order_relaxed);
    if (config_.enableRealtimeMetrics)
        ++metrics_.recoveryAttempts;

    // Drop both streams unconditionally -- safe from any PCM state,
    // forces both handles back to SETUP.
    snd_pcm_drop(captureHandle_);
    snd_pcm_drop(playbackHandle_);

    // Prepare both independently (streams are always on separate devices).
    int err = snd_pcm_prepare(captureHandle_);
    if (err < 0) {
        lastAlsaError_ = err;
        lastErrorWasCapture_ = true;
        liveRecoveryFailures_.fetch_add(1, std::memory_order_relaxed);
        if (config_.enableRealtimeMetrics)
            ++metrics_.recoveryFailures;
        return false;
    }

    err = snd_pcm_prepare(playbackHandle_);
    if (err < 0) {
        lastAlsaError_ = err;
        lastErrorWasCapture_ = false;
        liveRecoveryFailures_.fetch_add(1, std::memory_order_relaxed);
        if (config_.enableRealtimeMetrics)
            ++metrics_.recoveryFailures;
        return false;
    }

    if (!primePlayback()) {
        liveRecoveryFailures_.fetch_add(1, std::memory_order_relaxed);
        if (config_.enableRealtimeMetrics)
            ++metrics_.recoveryFailures;
        return false;
    }
    lastAlsaError_ = 0;
    return true;
}

bool AlsaAudioEngine::primePlayback()
{
    // Fill the exact committed playback buffer size. The full-buffer start
    // threshold starts playback after the final write.
    unsigned int remaining = playbackSettings_.bufferFrames;
    while (remaining > 0) {
        const unsigned int frames = std::min(remaining,
                                             playbackSettings_.periodFrames);
        if (!writePlaybackBlock(silenceRaw_.data(), static_cast<int>(frames)))
            return false;
        remaining -= frames;
    }
    return true;
}

bool AlsaAudioEngine::readCaptureBlock(int frames)
{
    const int bytesPerFrame = captureChannels_ * bytesPerSample(captureFmt_);
    int offset = 0;
    int waitAttempts = 0;
    while (offset < frames) {
        const int remaining = frames - offset;
        auto* destination = captureRaw_.data()
            + static_cast<std::size_t>(offset) * bytesPerFrame;
        const snd_pcm_sframes_t result = snd_pcm_readi(
            captureHandle_, destination, static_cast<snd_pcm_uframes_t>(remaining));
        if (result > 0) {
            if (result != remaining && config_.enableRealtimeMetrics)
                ++metrics_.shortReads;
            offset += static_cast<int>(result);
            waitAttempts = 0;
            continue;
        }

        if (result == -EAGAIN) {
            if (!waitForIo(captureHandle_, true, waitAttempts))
                return false;
            continue;
        }

        // Zero frames is not useful progress. Treat it as a recoverable I/O
        // failure rather than spinning in the realtime loop.
        if (running_.load(std::memory_order_acquire))
            recordIoError(true, result < 0 ? static_cast<int>(result) : -EIO);
        return false;
    }
    return true;
}

bool AlsaAudioEngine::writePlaybackBlock(const void* data, int frames)
{
    const int bytesPerFrame = playbackChannels_ * bytesPerSample(playbackFmt_);
    int offset = 0;
    int waitAttempts = 0;
    while (offset < frames) {
        const int remaining = frames - offset;
        const auto* source = static_cast<const uint8_t*>(data)
            + static_cast<std::size_t>(offset) * bytesPerFrame;
        const snd_pcm_sframes_t result = snd_pcm_writei(
            playbackHandle_, source, static_cast<snd_pcm_uframes_t>(remaining));
        if (result > 0) {
            if (result != remaining && config_.enableRealtimeMetrics)
                ++metrics_.shortWrites;
            offset += static_cast<int>(result);
            waitAttempts = 0;
            continue;
        }

        if (result == -EAGAIN) {
            if (!waitForIo(playbackHandle_, false, waitAttempts))
                return false;
            continue;
        }

        // Zero frames is not useful progress. Treat it as a recoverable I/O
        // failure rather than spinning in the realtime loop.
        if (running_.load(std::memory_order_acquire))
            recordIoError(false, result < 0 ? static_cast<int>(result) : -EIO);
        return false;
    }
    return true;
}

bool AlsaAudioEngine::waitForIo(snd_pcm_t* handle, bool capture, int& waitAttempts)
{
    // A readiness wakeup can race with device state changes. Permit a small,
    // fixed number of retries, each with a finite timeout, then recover both
    // streams instead of allowing the realtime thread to hang indefinitely.
    constexpr int kMaxWaitAttempts = 4;
    if (++waitAttempts > kMaxWaitAttempts) {
        recordIoError(capture, -ETIMEDOUT);
        return false;
    }

    const int result = snd_pcm_wait(handle, ioWaitTimeoutMs_);
    if (result > 0)
        return running_.load(std::memory_order_acquire);

    if (running_.load(std::memory_order_acquire))
        recordIoError(capture, result < 0 ? result : -ETIMEDOUT);
    return false;
}

void AlsaAudioEngine::recordIoError(bool capture, int errorCode)
{
    lastAlsaError_ = errorCode;
    lastErrorWasCapture_ = capture;
    liveLastErrorCode_.store(errorCode, std::memory_order_relaxed);
    liveLastWasCapture_.store(capture, std::memory_order_relaxed);
    ioErrorCount_.fetch_add(1, std::memory_order_release);
    if (!config_.enableRealtimeMetrics)
        return;

    if (capture && errorCode == -EPIPE)
        ++metrics_.captureOverruns;
    if (!capture && errorCode == -EPIPE)
        ++metrics_.playbackUnderruns;
}

uint64_t AlsaAudioEngine::monotonicRawNs() noexcept
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(now.tv_nsec);
}

void AlsaAudioEngine::recordDuration(std::array<uint32_t, kMetricBins>& histogram,
                                     uint64_t durationNs, uint64_t& totalNs,
                                     uint64_t& maxNs)
{
    totalNs += durationNs;
    maxNs = std::max(maxNs, durationNs);
    const std::size_t bin = std::min<std::size_t>(
        durationNs / kMetricBinNs, histogram.size() - 1);
    ++histogram[bin];
}

uint64_t AlsaAudioEngine::percentileNs(
    const std::array<uint32_t, kMetricBins>& histogram,
    uint64_t count, unsigned int perThousand) noexcept
{
    if (count == 0)
        return 0;

    const uint64_t rank = (count * perThousand + 999) / 1000;
    uint64_t accumulated = 0;
    for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
        accumulated += histogram[bin];
        if (accumulated >= rank)
            return static_cast<uint64_t>(bin) * kMetricBinNs;
    }
    return static_cast<uint64_t>(histogram.size() - 1) * kMetricBinNs;
}

AudioEngine::RealtimeMetrics AlsaAudioEngine::realtimeMetrics() const
{
    RealtimeMetrics result;
    result.enabled = config_.enableRealtimeMetrics;
    if (!result.enabled)
        return result;

    result.processedBlocks = metrics_.blockCount;
    if (metrics_.blockCount > 0) {
        result.processingMeanNs = metrics_.processingTotalNs / metrics_.blockCount;
        result.processingP95Ns = percentileNs(metrics_.processingHistogram,
                                              metrics_.blockCount, 950);
        result.processingP99Ns = percentileNs(metrics_.processingHistogram,
                                              metrics_.blockCount, 990);
        result.processingP999Ns = percentileNs(metrics_.processingHistogram,
                                               metrics_.blockCount, 999);
        result.processingMaxNs = metrics_.processingMaxNs;
        result.callbackMeanNs = metrics_.callbackTotalNs / metrics_.blockCount;
        result.callbackP95Ns = percentileNs(metrics_.callbackHistogram,
                                            metrics_.blockCount, 950);
        result.callbackP99Ns = percentileNs(metrics_.callbackHistogram,
                                            metrics_.blockCount, 990);
        result.callbackP999Ns = percentileNs(metrics_.callbackHistogram,
                                             metrics_.blockCount, 999);
        result.callbackMaxNs = metrics_.callbackMaxNs;
    }
    result.deadlineMisses = metrics_.deadlineMisses;
    result.captureOverruns = metrics_.captureOverruns;
    result.playbackUnderruns = metrics_.playbackUnderruns;
    result.shortReads = metrics_.shortReads;
    result.shortWrites = metrics_.shortWrites;
    result.recoveryAttempts = metrics_.recoveryAttempts;
    result.recoveryFailures = metrics_.recoveryFailures;
    return result;
}

AlsaAudioEngine::IoStatus AlsaAudioEngine::ioStatus() const noexcept
{
    IoStatus result;
    result.errors = ioErrorCount_.load(std::memory_order_acquire);
    result.recoveryAttempts = liveRecoveryAttempts_.load(std::memory_order_relaxed);
    result.recoveryFailures = liveRecoveryFailures_.load(std::memory_order_relaxed);
    result.lastErrorCode = liveLastErrorCode_.load(std::memory_order_relaxed);
    result.lastWasCapture = liveLastWasCapture_.load(std::memory_order_relaxed);
    return result;
}

// ---------------------------------------------------------------------------
// Format conversion helpers
// ---------------------------------------------------------------------------

void AlsaAudioEngine::deinterleaveCapture(const void* raw, float* mono,
                                            int frames, int totalChannels,
                                            int channel)
{
    switch (captureFmt_) {
        case SampleFormat::Int24Packed: {
            const uint8_t* src = static_cast<const uint8_t*>(raw);
            constexpr float kScale = 1.f / 8388608.f; // 2^23

            for (int i = 0; i < frames; ++i) {
                const int sampleIndex = i * totalChannels + channel;
                const uint8_t* p = src + sampleIndex * 3;

                int32_t value =
                    static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);

                // Sign-extend 24-bit signed value to 32 bits.
                if (value & 0x00800000)
                    value |= 0xFF000000;

                mono[i] = static_cast<float>(value) * kScale;
            }
            break;
        }
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
        case SampleFormat::Int24Packed: {
            uint8_t* dst = static_cast<uint8_t*>(raw);

            std::memset(
                dst,
                0,
                static_cast<std::size_t>(frames) * totalChannels * 3);

            constexpr float kScale = 8388607.f; // 2^23 - 1

            for (int i = 0; i < frames; ++i) {
                for (int c = 0; c < totalChannels; ++c) {
                    if (!(channelMask & (1 << c)))
                        continue;

                    float sample = mono[i];

                    // Clip to valid PCM range.
                    if (sample > 1.f)
                        sample = 1.f;
                    else if (sample < -1.f)
                        sample = -1.f;

                    const int32_t value =
                        static_cast<int32_t>(sample * kScale);

                    const std::size_t sampleIndex =
                        static_cast<std::size_t>(i) * totalChannels + c;

                    uint8_t* p = dst + sampleIndex * 3;

                    p[0] = static_cast<uint8_t>(value & 0xff);
                    p[1] = static_cast<uint8_t>((value >> 8) & 0xff);
                    p[2] = static_cast<uint8_t>((value >> 16) & 0xff);
                }
            }
            break;
        }
        case SampleFormat::Int16: {
            int16_t* dst = static_cast<int16_t*>(raw);
            std::memset(dst, 0,
                static_cast<std::size_t>(frames) * totalChannels * sizeof(int16_t));
            constexpr float kScale = 32767.f;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < totalChannels; ++c)
                    if (channelMask & (1 << c))
                        dst[i * totalChannels + c] =
                            static_cast<int16_t>(
                                std::clamp(mono[i], -1.f, 1.f) * kScale);
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
                            static_cast<int32_t>(
                                std::clamp(mono[i], -1.f, 1.f) * kScale);
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
