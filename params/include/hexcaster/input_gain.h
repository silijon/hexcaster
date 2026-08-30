#pragma once

#include <cstdint>

namespace hexcaster {

constexpr float kDefaultInterfaceInputLevelDBu = 11.63f;
constexpr float kDefaultInputTrimMinDb = -12.f;
constexpr float kDefaultInputTrimMaxDb = 12.f;

// Piecewise MIDI mapping with an exact center value at CC 64. This supports
// asymmetric parameter ranges and non-zero defaults.
constexpr float centeredValueFromMidiCc(uint8_t value,
                                        float minimum,
                                        float center,
                                        float maximum) noexcept
{
    if (value == 0) return minimum;
    if (value == 64) return center;
    if (value == 127) return maximum;
    if (value <= 64)
        return minimum + (static_cast<float>(value) / 64.f) * (center - minimum);
    return center + (static_cast<float>(value - 64) / 63.f) * (maximum - center);
}

// MIDI has no single integer midpoint. Map each side independently so CC 64
// is exactly the model-reference setting (0 dB user trim).
constexpr float inputTrimDbFromMidiCc(
    uint8_t value,
    float minimumDb = kDefaultInputTrimMinDb,
    float maximumDb = kDefaultInputTrimMaxDb) noexcept
{
    return centeredValueFromMidiCc(value, minimumDb, 0.f, maximumDb);
}

constexpr float effectivePreNamGainDb(float modelCalibrationDb,
                                      float userTrimDb) noexcept
{
    return modelCalibrationDb + userTrimDb;
}

} // namespace hexcaster
