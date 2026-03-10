#pragma once

#include <functional>
#include <string>
#include <cstdint>

namespace hexcaster {

/**
 * AudioEngine: abstract base class for audio I/O backends.
 *
 * Backends:
 *   AlsaAudioEngine  -- direct ALSA, single or separate devices, lowest latency
 *   JackAudioEngine  -- JACK/PipeWire client, multi-device clock sync (future)
 *
 * Usage:
 *   AlsaAudioEngine engine;
 *   engine.open(config);
 *   engine.setCallback([&](float* buf, int n){ pipeline.process(buf, n); });
 *   engine.run();   // blocks until stop() is called
 *   engine.close();
 */
class AudioEngine {
public:
    /**
     * ProcessCallback: called once per audio block from the RT thread.
     * The buffer is mono float, in-place -- modify it and return.
     * Must be real-time safe: no allocation, no blocking, no I/O.
     */
    using ProcessCallback = std::function<void(float* buffer, int numFrames)>;

    /**
     * Config: parameters for opening the audio engine.
     *
     * For single-device operation:
     *   set inputDevice = outputDevice = "hw:2,0"
     *
     * For separate devices:
     *   set inputDevice  = "hw:2,0"
     *   set outputDevice = "hw:3,0"
     *   Note: clock drift recovery is not yet implemented.
     *
     * inputChannel:   which channel of a stereo interface to use as input (0=L, 1=R)
     * outputChannels: bitmask of output channels to write to (0x1=L, 0x2=R, 0x3=both)
     */
    struct Config {
        std::string  inputDevice    = "hw:2,0";
        std::string  outputDevice   = "hw:2,0";
        unsigned int sampleRate     = 48000;
        unsigned int bufferFrames   = 128;
        unsigned int periods        = 2;
        int          inputChannel   = 0;    // 0=left, 1=right
        int          outputChannels = 0x3;  // bitmask: 0x1=L, 0x2=R, 0x3=both
    };

    virtual ~AudioEngine() = default;

    /**
     * Open and configure the audio device(s).
     * Not real-time safe. Must be called before run().
     * Returns true on success. On failure, errorMessage() contains details.
     */
    virtual bool open(const Config& config) = 0;

    /**
     * Set the processing callback. Must be called before run().
     */
    virtual void setCallback(ProcessCallback cb) = 0;

    /**
     * Start the audio loop. Blocks until stop() is called or a fatal error occurs.
     * Sets real-time thread priority internally (best-effort; continues if not granted).
     */
    virtual void run() = 0;

    /**
     * Signal the audio loop to stop. Safe to call from a signal handler.
     * run() will return shortly after.
     */
    virtual void stop() = 0;

    /**
     * Close the audio device(s) and release resources.
     * Must not be called from the audio thread.
     */
    virtual void close() = 0;

    /**
     * Returns the last error message, or empty string if no error.
     */
    virtual const std::string& errorMessage() const = 0;

    /**
     * Returns the actual sample rate negotiated with the device.
     * Valid after open().
     */
    virtual unsigned int actualSampleRate() const = 0;

    /**
     * Returns the actual buffer size (frames per period) negotiated with the device.
     * Valid after open().
     */
    virtual unsigned int actualBufferFrames() const = 0;
};

} // namespace hexcaster
