#pragma once

#include "hexcaster/param_id.h"
#include "hexcaster/param_registry.h"

#include <string>
#include <functional>

namespace hexcaster::tui {

/**
 * MeterData: a single snapshot of all observable DSP values.
 *
 * Captured once per TUI frame (~30 Hz) on the TUI thread by calling
 * atomic loads on ParamRegistry and component observation atomics.
 * Never touched by the audio thread after construction.
 */
struct MeterData {
    // --- Gate ---
    float gateGain        = 0.f;   // [0, 1] current gate openness (observation atomic)
    int   gateState       = 0;     // NoiseGate::State as int (observation atomic)

    // --- All ParamRegistry values (read each frame) ---
    float noiseGateThreshold = 0.f; // dB
    float noiseGateAttack    = 0.f; // ms
    float noiseGateRelease   = 0.f; // ms
    float noiseGateHold      = 0.f; // ms

    float inputGain          = 0.f; // dB
    float masterVolume       = 0.f; // dB

    float bloomEnvelope      = 0.f; // [0, 1] (observation atomic)
    float bloomBasePre       = 0.f; // dB
    float bloomBasePost      = 0.f; // dB
    float bloomDepth         = 0.f; // dB
    float bloomCompensation  = 0.f; // ratio
    float bloomSensitivity   = 0.f; // dB
    float envAttack          = 0.f; // ms
    float envRelease         = 0.f; // ms

    float eqGain             = 0.f; // dB
    float eqSweep            = 0.f; // Hz
    float eqQ                = 0.f; // dimensionless

    // --- Signal levels (host-level peak metering, dB) ---
    // Range: -60 dB (floor/silence) to 0 dB (full scale).
    // Measured in the audio callback: input before pipeline, output after.
    float inputLevelDb  = -60.f;
    float outputLevelDb = -60.f;

    // Set once at startup, never changes during a session
    std::string modelName;
};

/**
 * MeterDesc: metadata describing a single meter widget.
 *
 * The TUI screens define their meters as arrays of these descriptors.
 * Ranges are always sourced from ParamRegistry::getRange() for
 * ParamId-backed meters, ensuring a single source of truth.
 *
 * For observation-only meters (e.g. envelope, gate gain) that are not
 * in the registry, paramId is ParamId::kCount (sentinel) and
 * valueGetter provides the value directly.
 */
struct MeterDesc {
    const char* label;

    // If paramId != ParamId::kCount, this meter is backed by a registry param.
    // Range is fetched from registry.getRange(paramId).
    // Value is fetched from registry.get(paramId).
    // j/k can write via registry.set(paramId, ...).
    ParamId paramId = ParamId::kCount;

    // For observation-only meters: provides the current value [0, 1] directly.
    // Ignored when paramId != kCount.
    std::function<float(const MeterData&)> valueGetter;

    // When true: j/k adjustment is blocked regardless of MIDI mapping.
    // Set for observation-only meters (envelope, gate gain/state).
    bool alwaysReadOnly = false;

    // Unit label shown alongside the numeric value (e.g. " dB", " Hz", "")
    const char* unit = "";

    // --- Factory helpers ---

    /** Create a registry-backed meter. */
    static MeterDesc fromParam(const char* label, ParamId id, const char* unit = "") {
        MeterDesc d;
        d.label   = label;
        d.paramId = id;
        d.unit    = unit;
        return d;
    }

    /** Create a read-only observation meter with a fixed [0,1] value range. */
    static MeterDesc fromObservation(const char* label,
                                     std::function<float(const MeterData&)> getter,
                                     const char* unit = "") {
        MeterDesc d;
        d.label          = label;
        d.paramId        = ParamId::kCount;
        d.valueGetter    = std::move(getter);
        d.alwaysReadOnly = true;
        d.unit           = unit;
        return d;
    }

    bool isRegistryParam() const { return paramId != ParamId::kCount; }
};

} // namespace hexcaster::tui
