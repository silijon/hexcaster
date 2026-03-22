#include "tui.h"
#include "screens/main_screen.h"
#include "screens/eq_screen.h"
#include "screens/bloom_screen.h"
#include "screens/meter_widget.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace hexcaster::tui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Tui::Tui(std::function<MeterData()> snapshotFn,
         ParamRegistry&             registry,
         const MidiMap&             midiMap,
         std::atomic<bool>&         quitFlag)
    : snapshotFn_(std::move(snapshotFn))
    , registry_(registry)
    , midiMap_(midiMap)
    , quitFlag_(quitFlag)
    , screen_(ftxui::ScreenInteractive::Fullscreen())
{
    buildScreenDefs();
}

Tui::~Tui()
{
    stopRefreshThread();
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void Tui::run()
{
    startRefreshThread();

    auto root = buildRoot();
    screen_.Loop(root);

    stopRefreshThread();
}

// ---------------------------------------------------------------------------
// Screen definitions
// ---------------------------------------------------------------------------

void Tui::buildScreenDefs()
{
    screens_.clear();

    // --- Main screen ---
    {
        ScreenDef s;
        s.name = "Main";
        s.meters = buildMainScreenMeters();
        screens_.push_back(std::move(s));
    }

    // --- EQ screen ---
    {
        ScreenDef s;
        s.name = "EQ";
        s.meters = buildEqScreenMeters();
        screens_.push_back(std::move(s));
    }

    // --- Bloom screen ---
    {
        ScreenDef s;
        s.name = "Bloom";
        s.meters = buildBloomScreenMeters();
        screens_.push_back(std::move(s));
    }
}

// ---------------------------------------------------------------------------
// Refresh thread (30 Hz)
// ---------------------------------------------------------------------------

void Tui::startRefreshThread()
{
    refreshRunning_.store(true);
    refreshThread_ = std::thread([this] {
        using namespace std::chrono;
        constexpr auto kInterval = milliseconds(33); // ~30 fps

        while (refreshRunning_.load(std::memory_order_relaxed)) {
            // Capture snapshot on the refresh thread (lock-free atomic reads)
            auto snap = snapshotFn_();

            // Post state update to the screen thread (thread-safe FTXUI API)
            screen_.Post([this, snap = std::move(snap)] {
                snapshot_ = snap;

                // If the external quit flag was set (e.g. Ctrl+C), exit TUI
                if (quitFlag_.load(std::memory_order_relaxed)) {
                    screen_.Exit();
                }
            });

            // Trigger a redraw
            screen_.Post(ftxui::Event::Custom);

            std::this_thread::sleep_for(kInterval);
        }
    });
}

void Tui::stopRefreshThread()
{
    refreshRunning_.store(false);
    if (refreshThread_.joinable()) {
        refreshThread_.join();
    }
}

// ---------------------------------------------------------------------------
// FTXUI component tree
// ---------------------------------------------------------------------------

ftxui::Component Tui::buildRoot()
{
    // Use a CatchEvent component so we handle all keyboard input ourselves.
    auto renderer = ftxui::Renderer([this] {
        auto helpBar = ftxui::hbox(ftxui::Elements{
            ftxui::text(" [ prev  "),
            ftxui::text("] next  "),
            ftxui::text("Tab sel  "),
            ftxui::text("j/k adj  "),
            ftxui::text("q quit"),
        }) | ftxui::dim;

        // Body: fixed signal panel on the left, current screen on the right
        auto body = ftxui::hbox(ftxui::Elements{
            renderSignalPanel(),
            ftxui::separator(),
            renderCurrentScreen() | ftxui::flex,
        });

        return ftxui::vbox(ftxui::Elements{
            renderHeader(),
            ftxui::separator(),
            body | ftxui::flex,
            ftxui::separator(),
            helpBar,
        });
    });

    return ftxui::CatchEvent(renderer, [this](ftxui::Event e) {
        return handleEvent(e);
    });
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

ftxui::Element Tui::renderHeader()
{
    // Screen tabs
    ftxui::Elements tabs;
    for (int i = 0; i < static_cast<int>(screens_.size()); ++i) {
        auto label = std::string(" ") + screens_[i].name + " ";
        if (i == currentScreen_) {
            tabs.push_back(ftxui::text(label) | ftxui::bold | ftxui::inverted);
        } else {
            tabs.push_back(ftxui::text(label));
        }
        if (i + 1 < static_cast<int>(screens_.size())) {
            tabs.push_back(ftxui::text(" "));
        }
    }

    // Model name: basename only
    std::string modelDisplay = snapshot_.modelName;
    if (!modelDisplay.empty()) {
        try {
            modelDisplay = std::filesystem::path(modelDisplay).filename().string();
        } catch (...) {}
    }
    if (modelDisplay.empty()) modelDisplay = "(no model)";

    auto titleRow = ftxui::hbox(ftxui::Elements{
        ftxui::text(" HexCaster") | ftxui::bold,
        ftxui::filler(),
        ftxui::text("model: ") | ftxui::dim,
        ftxui::text(modelDisplay),
        ftxui::text(" "),
    });
    auto tabRow = ftxui::hbox(ftxui::Elements{
        ftxui::text(" "),
        ftxui::hbox(tabs),
    });
    return ftxui::vbox(ftxui::Elements{ titleRow, tabRow });
}

// ---------------------------------------------------------------------------
// Signal panel (fixed left column -- always visible)
// ---------------------------------------------------------------------------

ftxui::Element Tui::renderSignalPanel()
{
    using namespace ftxui;

    auto inMeter  = makeSignalMeter("In",  snapshot_.inputLevelDb);
    auto outMeter = makeSignalMeter("Out", snapshot_.outputLevelDb);

    Elements rows;
    rows.push_back(text(" Sig ") | bold | hcenter);
    rows.push_back(text(""));
    rows.push_back(hbox(Elements{ inMeter, text(" "), outMeter }));
    return vbox(std::move(rows));
}

// ---------------------------------------------------------------------------
// Current screen
// ---------------------------------------------------------------------------

ftxui::Element Tui::renderCurrentScreen()
{
    switch (currentScreen_) {
        case 0: return renderMainScreen(snapshot_, currentMeters(), selectedMeter_,
                                        registry_, midiMap_);
        case 1: return renderEqScreen(snapshot_, currentMeters(), selectedMeter_,
                                      registry_, midiMap_);
        case 2: return renderBloomScreen(snapshot_, currentMeters(), selectedMeter_,
                                         registry_, midiMap_);
        default: return ftxui::text("unknown screen");
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

bool Tui::handleEvent(ftxui::Event e)
{
    // Quit
    if (e == ftxui::Event::Character('q')) {
        quitFlag_.store(true, std::memory_order_relaxed);
        screen_.Exit();
        return true;
    }

    // Screen navigation: ] = next, [ = previous
    if (e == ftxui::Event::Character(']')) {
        currentScreen_ = (currentScreen_ + 1) % static_cast<int>(screens_.size());
        selectedMeter_ = 0;
        return true;
    }
    if (e == ftxui::Event::Character('[')) {
        int n = static_cast<int>(screens_.size());
        currentScreen_ = (currentScreen_ + n - 1) % n;
        selectedMeter_ = 0;
        return true;
    }

    // Meter selection: Tab / Shift+Tab
    if (e == ftxui::Event::Tab) {
        int n = static_cast<int>(currentMeters().size());
        if (n > 0) selectedMeter_ = (selectedMeter_ + 1) % n;
        return true;
    }
    if (e == ftxui::Event::TabReverse) {
        int n = static_cast<int>(currentMeters().size());
        if (n > 0) selectedMeter_ = (selectedMeter_ + n - 1) % n;
        return true;
    }

    // Value adjustment: j/k (small), J/K (large)
    if (e == ftxui::Event::Character('j')) { adjustSelected(-1, false); return true; }
    if (e == ftxui::Event::Character('k')) { adjustSelected(+1, false); return true; }
    if (e == ftxui::Event::Character('J')) { adjustSelected(-1, true);  return true; }
    if (e == ftxui::Event::Character('K')) { adjustSelected(+1, true);  return true; }

    return false;
}

void Tui::adjustSelected(int direction, bool large)
{
    if (!selectedIsEditable()) return;

    const auto& meters = currentMeters();
    if (selectedMeter_ < 0 || selectedMeter_ >= static_cast<int>(meters.size())) return;

    const auto& m = meters[selectedMeter_];
    if (!m.isRegistryParam()) return;

    const auto range = registry_.getRange(m.paramId);
    const float span = range.max - range.min;
    const float step = large ? span * 0.10f : span * 0.02f;

    const float current = registry_.get(m.paramId);
    // registry_.set() clamps to [min, max] internally
    registry_.set(m.paramId, current + direction * step);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const std::vector<MeterDesc>& Tui::currentMeters() const
{
    return screens_[currentScreen_].meters;
}

float Tui::normalize(ParamId id, float value) const
{
    const auto range = registry_.getRange(id);
    if (range.max <= range.min) return 0.f;
    const float norm = (value - range.min) / (range.max - range.min);
    return std::clamp(norm, 0.f, 1.f);
}

bool Tui::selectedIsEditable() const
{
    const auto& meters = currentMeters();
    if (selectedMeter_ < 0 || selectedMeter_ >= static_cast<int>(meters.size())) return false;

    const auto& m = meters[selectedMeter_];
    if (m.alwaysReadOnly) return false;
    if (!m.isRegistryParam()) return false;
    if (midiMap_.isMapped(m.paramId)) return false;
    return true;
}

} // namespace hexcaster::tui
