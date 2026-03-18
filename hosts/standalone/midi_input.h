#pragma once

#include "hexcaster/midi_map.h"
#include "hexcaster/param_registry.h"

#include <atomic>
#include <string>
#include <thread>

// Forward-declare ALSA raw MIDI type
struct _snd_rawmidi;
typedef struct _snd_rawmidi snd_rawmidi_t;

namespace hexcaster {

/**
 * MidiInput: ALSA raw MIDI reader for a USB MIDI controller.
 *
 * Runs a dedicated reader thread that blocks on snd_rawmidi_read().
 * When a CC message arrives, it calls MidiMap::dispatch() which writes
 * the scaled value to ParamRegistry atomically.
 *
 * Thread model:
 *   - Reader thread: normal priority, blocked in snd_rawmidi_read().
 *   - Audio thread: reads ParamRegistry atomically -- no contention.
 *   - No locks in the dispatch path.
 *
 * MIDI parsing:
 *   - Only CC messages (status 0xBn, any channel) are dispatched.
 *   - All other message types (note on/off, PC, sysex, etc.) are ignored.
 *   - Running status is supported.
 *
 * Usage:
 *   MidiInput midi;
 *   if (midi.open("hw:1,0,0")) {
 *       midi.start(midiMap, paramRegistry);
 *       // ... audio running ...
 *       midi.stop();
 *       midi.close();
 *   }
 */
class MidiInput {
public:
    MidiInput() = default;
    ~MidiInput();

    MidiInput(const MidiInput&)            = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    /**
     * Open the ALSA raw MIDI device for input.
     * Not real-time safe.
     * Returns true on success; errorMessage() contains details on failure.
     */
    bool open(const std::string& device);

    /**
     * Start the reader thread.
     * MidiMap and ParamRegistry must outlive the MidiInput.
     */
    void start(MidiMap& midiMap, ParamRegistry& registry);

    /**
     * Signal the reader thread to stop and join it.
     * Blocks until the thread exits.
     */
    void stop();

    /**
     * Close the ALSA device. Call after stop().
     */
    void close();

    bool isOpen() const { return handle_ != nullptr; }
    const std::string& errorMessage() const { return errorMsg_; }

private:
    void readerLoop(MidiMap& midiMap, ParamRegistry& registry);

    snd_rawmidi_t*    handle_  = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
    std::string       errorMsg_;
};

} // namespace hexcaster
