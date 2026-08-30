#pragma once

#include <cstdint>

namespace hexcaster {

constexpr float kDefaultInterfaceInputLevelDBu = 11.63f;
constexpr float kDefaultInputTrimMinDb = -12.f;
constexpr float kDefaultInputTrimMaxDb = 12.f;

// MIDI has no single integer midpoint. Map each side independently so CC 64
// is exactly the model-reference setting (0 dB user trim).
constexpr float inputTrimDbFromMidiCc(
    uint8_t value,
    float minimumDb = kDefaultInputTrimMinDb,
    float maximumDb = kDefaultInputTrimMaxDb) noexcept
{
    if (value <= 64)
        return minimumDb + (static_cast<float>(value) / 64.f) * -minimumDb;
    return (static_cast<float>(value - 64) / 63.f) * maximumDb;
}

constexpr float effectivePreNamGainDb(float modelCalibrationDb,
                                      float userTrimDb) noexcept
{
    return modelCalibrationDb + userTrimDb;
}

} // namespace hexcaster
