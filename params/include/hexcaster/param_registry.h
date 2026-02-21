#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include "hexcaster/param_id.h"

namespace hexcaster {

/**
 * ParamRegistry: central store of all plugin parameters.
 *
 * Thread-safety model:
 *   - Control thread (host, MIDI, UI) calls set().
 *   - Audio thread calls get().
 *   - No locks. Each parameter is an independent std::atomic<float>.
 *
 * Parameters are identified by ParamId enum.
 * Default values are set in the constructor.
 */
class ParamRegistry {
public:
    ParamRegistry();

    /**
     * Write a parameter value. Called from control thread only.
     * Value is clamped to the registered range.
     */
    void set(ParamId id, float value);

    /**
     * Read a parameter value. Safe to call from audio thread.
     */
    float get(ParamId id) const;

    /**
     * Reset all parameters to their default values.
     * Not real-time safe (multiple atomic writes).
     */
    void resetToDefaults();

private:
    static constexpr int kNumParams = static_cast<int>(ParamId::kCount);

    struct ParamInfo {
        float defaultValue;
        float minValue;
        float maxValue;
    };

    static const std::array<ParamInfo, kNumParams> kParamInfo;

    std::array<std::atomic<float>, kNumParams> values_;
};

} // namespace hexcaster
