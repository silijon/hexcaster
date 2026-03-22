#pragma once

/**
 * meter_widget.h -- shared helper for rendering a single vertical meter.
 *
 * Provides:
 *   makeMeter()   -- build an ftxui::Element for one meter column
 *   gateStateStr()-- convert NoiseGate::State int to a short label
 *
 * Visual layout of a single meter column (height ~12 rows):
 *
 *   Label        <- parameter name (truncated to kMeterWidth)
 *   ┌───┐
 *   │███│        <- gaugeUp fill, coloured green/yellow/red by zone
 *   │   │
 *   └───┘
 *   -12.3 dB     <- current numeric value + unit
 *   [MIDI]       <- shown only if MIDI-mapped (read-only indicator)
 *   [>>]         <- shown only if this meter is selected
 */

#include "../meter_data.h"
#include "hexcaster/param_registry.h"
#include "hexcaster/midi_map.h"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace hexcaster::tui {

constexpr int kMeterBarWidth   = 5;   // character columns for the gauge bar
constexpr int kMeterBarHeight  = 12;  // character rows for the gauge bar

/**
 * Format a float value for display. Uses fixed-point with one decimal.
 * Trims trailing zeros but always shows the unit.
 */
inline std::string fmtValue(float v, const char* unit)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v;
    return ss.str() + unit;
}

/**
 * Choose bar color based on normalized position [0, 1].
 * Green below 70%, yellow 70-90%, red above 90%.
 * For bipolar params (EQ gain) the coloring is applied to absolute deviation.
 */
inline ftxui::Color barColor(float norm)
{
    if (norm >= 0.90f) return ftxui::Color::Red;
    if (norm >= 0.70f) return ftxui::Color::Yellow;
    return ftxui::Color::Green;
}

/**
 * Short string representation of gate state.
 * The int corresponds to NoiseGate::State cast to uint8_t.
 */
inline const char* gateStateStr(int s)
{
    switch (s) {
        case 0: return "CLOSED ";
        case 1: return "OPENING";
        case 2: return "OPEN   ";
        case 3: return "HOLDING";
        case 4: return "CLOSING";
        default: return "???    ";
    }
}

/**
 * Render one meter column.
 *
 * @param label       Parameter name (short)
 * @param normValue   Normalized position for the bar [0, 1]
 * @param displayVal  Formatted string for the numeric readout
 * @param isMidi      Whether the param is MIDI-mapped (shows [MIDI])
 * @param isSelected  Whether this meter is currently selected
 * @param isReadOnly  Whether the meter is always read-only (observation)
 */
inline ftxui::Element makeMeter(const std::string& label,
                                float              normValue,
                                const std::string& displayVal,
                                bool               isMidi,
                                bool               isSelected,
                                bool               isReadOnly)
{
    using namespace ftxui;

    normValue = std::clamp(normValue, 0.f, 1.f);
    const Color barCol = barColor(normValue);

    // The bar itself
    auto bar = gaugeUp(normValue)
             | color(barCol)
             | size(WIDTH,  EQUAL, kMeterBarWidth)
             | size(HEIGHT, EQUAL, kMeterBarHeight);

    // Framed bar
    auto framedBar = bar | border;

    // Status line: MIDI tag or read-only tag
    std::string statusLine;
    if (isMidi)          statusLine = "[MIDI] ";
    else if (isReadOnly) statusLine = "[read] ";
    else                 statusLine = "       ";

    // Selection indicator
    auto labelElem = text(label)
                   | (isSelected ? bold : nothing)
                   | (isSelected ? inverted : nothing);
    auto valueElem  = text(displayVal) | dim;
    auto statusElem = text(statusLine)
                    | (isMidi ? color(Color::Blue) : color(Color::GrayDark));
    auto selectElem = isSelected
                    ? (text("[sel]") | bold | color(Color::Cyan))
                    : text("     ");

    Elements rows;
    rows.push_back(labelElem | hcenter);
    rows.push_back(framedBar | hcenter);
    rows.push_back(valueElem | hcenter);
    rows.push_back(statusElem | hcenter);
    rows.push_back(selectElem | hcenter);
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, kMeterBarWidth + 4);
}

/**
 * Build a row of meter columns from a list of MeterDesc entries.
 * Handles normalization via registry ranges.
 */
inline ftxui::Element makeMeterRow(const std::vector<MeterDesc>& meters,
                                   int                           selectedIdx,
                                   const MeterData&              data,
                                   const ParamRegistry&          registry,
                                   const MidiMap&                midiMap)
{
    using namespace ftxui;

    Elements cols;
    cols.push_back(text(" "));  // left margin

    for (int i = 0; i < static_cast<int>(meters.size()); ++i) {
        const auto& m = meters[i];
        const bool  selected = (i == selectedIdx);

        float normValue   = 0.f;
        float rawValue    = 0.f;
        bool  midi        = false;
        bool  readOnly    = m.alwaysReadOnly;

        if (m.isRegistryParam()) {
            rawValue  = registry.get(m.paramId);
            auto range = registry.getRange(m.paramId);
            if (range.max > range.min)
                normValue = std::clamp((rawValue - range.min) / (range.max - range.min), 0.f, 1.f);
            midi = midiMap.isMapped(m.paramId);
        } else {
            // Observation getter: value already in [0, 1]
            rawValue  = m.valueGetter(data);
            normValue = std::clamp(rawValue, 0.f, 1.f);
            readOnly  = true;
        }

        const std::string disp = fmtValue(rawValue, m.unit);

        cols.push_back(makeMeter(m.label, normValue, disp, midi, selected, readOnly));
        cols.push_back(text("  "));  // column gap
    }

    return hbox(std::move(cols));
}

} // namespace hexcaster::tui
