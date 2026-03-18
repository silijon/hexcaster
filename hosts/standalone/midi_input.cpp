#include "midi_input.h"

#include <alsa/asoundlib.h>
#include <cstdio>

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
    // Open for input only (nullptr for output handle)
    int err = snd_rawmidi_open(&handle_, nullptr, device.c_str(), SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
        errorMsg_ = std::string("Failed to open MIDI device '")
                  + device + "': " + snd_strerror(err);
        handle_ = nullptr;
        return false;
    }

    // Switch to blocking mode for the reader thread.
    // We opened non-blocking first to get a fast failure if the device
    // doesn't exist, then switch to blocking for the read loop.
    snd_rawmidi_nonblock(handle_, 0);

    std::fprintf(stderr, "MIDI input: opened %s\n", device.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// start() / stop()
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
    running_.store(false, std::memory_order_release);

    // Unblock the read call by dropping the handle if thread is stuck.
    // snd_rawmidi_drop() wakes any blocked read.
    if (handle_) snd_rawmidi_drop(handle_);

    if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

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
// MIDI CC message format:
//   Byte 0: 0xBn  (status: Control Change, channel n)
//   Byte 1: cc    (controller number, 0-127)
//   Byte 2: value (controller value, 0-127)
//
// We implement "running status": if the next byte is a data byte (bit 7 = 0)
// and the last status byte was a CC, we reuse the previous status byte.
// ---------------------------------------------------------------------------

void MidiInput::readerLoop(MidiMap& midiMap, ParamRegistry& registry)
{
    // Parser state
    enum class State { WaitStatus, WaitData1, WaitData2 };
    State   state      = State::WaitStatus;
    uint8_t status     = 0;
    uint8_t data1      = 0;
    bool    isCC       = false;

    uint8_t byte = 0;

    while (running_.load(std::memory_order_acquire)) {
        ssize_t n = snd_rawmidi_read(handle_, &byte, 1);

        if (n < 0) {
            if (n == -EINTR) continue;   // interrupted by signal, retry
            if (!running_.load(std::memory_order_acquire)) break;
            std::fprintf(stderr, "MIDI read error: %s\n", snd_strerror(static_cast<int>(n)));
            break;
        }
        if (n == 0) continue;

        if (byte & 0x80) {
            // Status byte -- start of a new message
            status = byte;
            isCC   = ((status & 0xF0) == 0xB0);  // CC on any channel
            state  = isCC ? State::WaitData1 : State::WaitStatus;
        } else {
            // Data byte -- running status applies
            switch (state) {
                case State::WaitStatus:
                    // Unexpected data byte with no active CC status -- ignore
                    break;

                case State::WaitData1:
                    data1 = byte;
                    state = State::WaitData2;
                    break;

                case State::WaitData2:
                    if (isCC) {
                        const uint8_t cc    = data1 & 0x7F;
                        const uint8_t value = byte  & 0x7F;
                        midiMap.dispatch(cc, value, registry);
                    }
                    // Running status: stay in WaitData1 for the next message
                    // using the same status byte.
                    state = State::WaitData1;
                    break;
            }
        }
    }

    std::fprintf(stderr, "MIDI reader thread exited.\n");
}

} // namespace hexcaster
