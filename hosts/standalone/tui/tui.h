#pragma once

#include "meter_data.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace hexcaster::tui {

/**
 * Tui: the terminal user interface for the HexCaster standalone host.
 *
 * Runs on the main thread (FTXUI owns the terminal). The audio engine
 * runs on a background thread when TUI mode is active.
 *
 * Architecture:
 *   - A snapshot producer function (provided by main.cpp) reads all
 *     DSP observation values via atomic loads and returns a MeterData.
 *   - A 30 Hz refresh thread calls the producer, updates shared state,
 *     and posts Event::Custom to the FTXUI screen to trigger a redraw.
 *   - Keyboard input is handled by FTXUI's event loop on the main thread.
 *   - Parameter writes (j/k adjustment) go through ParamRegistry::set(),
 *     the same lock-free path used by MIDI CC.
 *   - MIDI-mapped parameters are marked read-only via MidiMap::isMapped().
 *
 * Screens:
 *   0: Main  -- noise gate, input gain, master volume
 *   1: EQ    -- eq gain, sweep, Q
 *   2: Bloom -- envelope, pre/post gain, depth, compensation, sensitivity
 *
 * Keyboard shortcuts (vim-style):
 *   q         -- quit
 *   } / {     -- next / previous screen (Shift+] / Shift+[)
 *   Tab       -- select next meter on current screen
 *   Shift+Tab -- select previous meter
 *   j / k     -- decrease / increase selected meter (small step: 2% of range)
 *   J / K     -- decrease / increase selected meter (large step: 10% of range)
 */
class Tui {
public:
    /**
     * @param snapshotFn   Called ~30x/s on the TUI refresh thread.
     *                     Must be lock-free / atomic-safe only.
     * @param registry     ParamRegistry for reading ranges and writing values.
     * @param midiMap      Used to mark MIDI-controlled params as read-only.
     * @param quitFlag     Shared atomic quit flag (also set by signal handler).
     *                     Tui sets it to true on 'q' keypress.
     */
    Tui(std::function<MeterData()>  snapshotFn,
        ParamRegistry&              registry,
        const MidiMap&              midiMap,
        std::atomic<bool>&          quitFlag);

    ~Tui();

    /**
     * Run the TUI event loop. Blocks until the user quits or quitFlag is set.
     * Must be called from the main thread.
     */
    void run();

private:
    // --- Dependencies ---
    std::function<MeterData()> snapshotFn_;
    ParamRegistry&             registry_;
    const MidiMap&             midiMap_;
    std::atomic<bool>&         quitFlag_;

    // --- FTXUI screen (owns the terminal) ---
    ftxui::ScreenInteractive   screen_;

    // --- Shared state (written by refresh thread, read by render on screen thread) ---
    MeterData                  snapshot_;   // latest data snapshot

    // --- Navigation state (screen thread only) ---
    int  currentScreen_  = 0;   // 0=Main, 1=EQ, 2=Bloom
    int  selectedMeter_  = 0;   // index within current screen's meter list

    // --- 30 Hz refresh thread ---
    std::thread                refreshThread_;
    std::atomic<bool>          refreshRunning_{ false };

    void startRefreshThread();
    void stopRefreshThread();

    // --- Rendering ---
    ftxui::Component buildRoot();
    ftxui::Element   renderHeader();
    ftxui::Element   renderCurrentScreen();

    // --- Input handling ---
    bool handleEvent(ftxui::Event event);
    void adjustSelected(int direction, bool large);

    // --- Meter helpers ---
    struct ScreenDef {
        const char*              name;
        std::vector<MeterDesc>   meters;
    };

    std::vector<ScreenDef> screens_;
    void buildScreenDefs();

    // Returns the meter list for the current screen
    const std::vector<MeterDesc>& currentMeters() const;

    // Normalize a registry param value to [0, 1] using its registered range
    float normalize(ParamId id, float value) const;

    // True if the selected meter may be adjusted (not MIDI-mapped or observation-only)
    bool selectedIsEditable() const;
};

} // namespace hexcaster::tui
