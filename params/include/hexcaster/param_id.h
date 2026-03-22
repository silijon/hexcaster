#pragma once

#include <cstdint>
#include <string_view>

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
    BloomBasePre_dB     = 0,  // Baseline pre-amp gain offset (dB)
    BloomBasePost_dB    = 1,  // Baseline post-amp gain offset (dB)
    BloomDepth_dB       = 2,  // Max input gain reduction at full envelope (dB)
    BloomCompensation   = 3,  // Output compensation ratio on input reduction [0, 2]
    BloomAttackMs       = 4,  // Bloom gain envelope attack time (ms)
    BloomReleaseMs      = 5,  // Bloom gain envelope release time (ms)
    BloomSensitivity_dB = 6,  // Detection signal gain before envelope follower (dB)

    // --- Input Gain ---
    InputGain_dB        = 30,

    // --- Noise Gate ---
    NoiseGateThreshold_dB = 50,  // Gate opens above this level [-80, 0] dB
    NoiseGateAttackMs     = 51,  // Time to open once signal exceeds threshold
    NoiseGateReleaseMs    = 52,  // Time to close once signal drops below threshold
    NoiseGateHoldMs       = 53,  // Minimum open time after signal drops below threshold

    // --- Mid-Sweep EQ ---
    EqGain_dB             = 60,  // Peaking filter boost/cut [-12, +12] dB
    EqSweepHz             = 61,  // Center frequency [300, 2500] Hz
    EqQ                   = 62,  // Bandwidth (Q factor) [0.3, 3.0], default 0.8

    // --- Master Volume ---
    MasterVolume_dB       = 70,  // Final output level before power amp [-60, +24] dB

    kCount              // Always last
};

/**
 * Look up a ParamId by its string name (e.g. "InputGain_dB").
 * Returns true and sets `out` on success.
 * Used by CLI parsers and config file loaders.
 */
inline bool paramIdFromName(std::string_view name, ParamId& out)
{
    struct Entry { std::string_view name; ParamId id; };
    static constexpr Entry kTable[] = {
        { "BloomBasePre_dB",    ParamId::BloomBasePre_dB    },
        { "BloomBasePost_dB",   ParamId::BloomBasePost_dB   },
        { "BloomDepth_dB",      ParamId::BloomDepth_dB      },
        { "BloomCompensation",  ParamId::BloomCompensation  },
        { "BloomAttackMs",      ParamId::BloomAttackMs      },
        { "BloomReleaseMs",     ParamId::BloomReleaseMs     },
        { "BloomSensitivity_dB",ParamId::BloomSensitivity_dB},
        { "InputGain_dB",     ParamId::InputGain_dB     },
        { "NoiseGateThreshold_dB",  ParamId::NoiseGateThreshold_dB  },
        { "NoiseGateAttackMs",      ParamId::NoiseGateAttackMs      },
        { "NoiseGateReleaseMs",     ParamId::NoiseGateReleaseMs     },
        { "NoiseGateHoldMs",        ParamId::NoiseGateHoldMs        },
        { "EqGain_dB",              ParamId::EqGain_dB              },
        { "EqSweepHz",              ParamId::EqSweepHz              },
        { "EqQ",                    ParamId::EqQ                    },
        { "MasterVolume_dB",        ParamId::MasterVolume_dB        },
    };
    for (auto& e : kTable) {
        if (e.name == name) { out = e.id; return true; }
    }
    return false;
}

} // namespace hexcaster
