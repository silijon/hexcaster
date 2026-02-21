#pragma once

#include <cstdint>

namespace hexcaster {

/**
 * ParamId: canonical enumeration of all plugin parameters.
 *
 * This is the contract between the control layer (hosts, MIDI mapping)
 * and the DSP layer. Add new parameters here first, then wire them up
 * in ParamRegistry and the relevant DSP stages.
 *
 * Convention:
 *   - Gain parameters in dB
 *   - Normalised [0,1] parameters use suffix _Norm
 *   - Time parameters in milliseconds
 */
enum class ParamId : uint32_t {
    // --- Bloom (dynamic gain) ---
    BloomBasePre_dB     = 0,  // Base pre-amp gain (dB)
    BloomBasePost_dB    = 1,  // Base post-amp gain (dB)
    BloomPreDepth       = 2,  // A: pre-gain reduction depth (dB per envelope unit)
    BloomPostDepth      = 3,  // B: post-gain compensation depth (dB per envelope unit)
    EnvAttackMs         = 4,
    EnvReleaseMs        = 5,

    // --- Post-distortion EQ ---
    EqBand1Freq         = 10,
    EqBand1GainDb       = 11,
    EqBand1Q            = 12,
    EqBand2Freq         = 13,
    EqBand2GainDb       = 14,
    EqBand2Q            = 15,
    EqBand3Freq         = 16,
    EqBand3GainDb       = 17,
    EqBand3Q            = 18,

    // --- Master ---
    MasterGain_dB       = 30,

    // --- Reverb ---
    ReverbRoomSize      = 40,
    ReverbDamping       = 41,
    ReverbWet_Norm      = 42,

    kCount              // Always last
};

} // namespace hexcaster
