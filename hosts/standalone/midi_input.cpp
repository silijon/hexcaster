#include "midi_input.h"

#include <alsa/asoundlib.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>

namespace hexcaster {

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

MidiInput::~MidiInput()
{
    stop();
    close();
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------

bool MidiInput::open(const std::string& device)
{
    // Open non-blocking. The reader loop uses poll() with a timeout to
    // wait for data, so we never need blocking reads.
    int err = snd_rawmidi_open(&handle_, nullptr, device.c_str(), SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
        errorMsg_ = std::string("Failed to open MIDI device '")
                  + device + "': " + snd_strerror(err);
        handle_ = nullptr;
        return false;
    }

    std::fprintf(stderr, "MIDI input: opened %s\n", device.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// start() / stop() / close()
// ---------------------------------------------------------------------------

void MidiInput::start(MidiMap& midiMap, ParamRegistry& registry)
{
    if (!handle_) return;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, &midiMap, &registry]() {
        readerLoop(midiMap, registry);
    });
}

void MidiInput::stop()
{
    if (!running_.load(std::memory_order_acquire)) return;
    // Signal the reader loop to exit. It checks running_ after each
    // poll() timeout (100 ms), so it will exit within ~100 ms.
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void MidiInput::close()
{
    if (handle_) {
        snd_rawmidi_close(handle_);
        handle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// readerLoop()
//
// MIDI byte parser state machine.
//
// Uses poll() with a 100 ms timeout so the loop can check running_ without
// blocking forever waiting for MIDI data. This makes stop() reliable: just
// set running_ = false and join -- the thread exits within ~100 ms.
//
// MIDI CC message format:
//   Byte 0: 0xBn  (status: Control Change, channel n)
//   Byte 1: cc    (controller number, 0-127)
//   Byte 2: value (controller value, 0-127)
//
// Running status is supported: consecutive CC messages may omit the
// status byte and reuse the previous one.
// ---------------------------------------------------------------------------

void MidiInput::readerLoop(MidiMap& midiMap, ParamRegistry& registry)
{
    // Get the poll file descriptor for this MIDI handle.
    struct pollfd pfd{};
    const int nfds = snd_rawmidi_poll_descriptors(handle_, &pfd, 1);
    if (nfds < 1) {
        std::fprintf(stderr, "MIDI: failed to get poll descriptor\n");
        return;
    }

    // Parser state
    enum class State { WaitStatus, WaitData1, WaitData2 };
    State   state = State::WaitStatus;
    uint8_t data1 = 0;
    bool    isCC  = false;

    while (running_.load(std::memory_order_acquire)) {
        // Wait up to 100 ms for MIDI data. On timeout, loop and re-check
        // running_. This is the only blocking call -- safe to exit from.
        const int ret = poll(&pfd, 1, 100 /*ms*/);

        if (ret < 0) {
            if (errno == EINTR) continue;  // interrupted by signal, retry
            std::fprintf(stderr, "MIDI poll error: %s\n", std::strerror(errno));
            break;
        }
        if (ret == 0) continue;  // timeout -- check running_ again

        // Data available -- drain all available bytes in this pass.
        uint8_t byte;
        while (true) {
            const ssize_t n = snd_rawmidi_read(handle_, &byte, 1);

            if (n == -EAGAIN) break;          // no more bytes right now
            if (n < 0) {
                if (!running_.load(std::memory_order_acquire)) goto done;
                std::fprintf(stderr, "MIDI read error: %s\n",
                             snd_strerror(static_cast<int>(n)));
                goto done;
            }
            if (n == 0) break;

            if (byte & 0x80) {
                // Status byte
                isCC  = ((byte & 0xF0) == 0xB0);  // CC on any channel
                state = isCC ? State::WaitData1 : State::WaitStatus;
            } else {
                // Data byte (running status)
                switch (state) {
                    case State::WaitStatus:
                        break;  // no active CC status -- ignore
                    case State::WaitData1:
                        data1 = byte;
                        state = State::WaitData2;
                        break;
                    case State::WaitData2:
                        if (isCC) {
                            midiMap.dispatch(data1 & 0x7F, byte & 0x7F, registry);
                        }
                        state = State::WaitData1;  // running status
                        break;
                }
            }
        }
    }

done:
    std::fprintf(stderr, "MIDI reader thread exited.\n");
}

} // namespace hexcaster
