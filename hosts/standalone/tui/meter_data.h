#pragma once

#include "hexcaster/param_id.h"
#include "hexcaster/param_registry.h"

#include <functional>
#include <optional>
#include <string>

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

    float bloomDetectorEnv     = 0.f;   // [0, 1] fast audio detector (observation atomic)
    float bloomEnvelope        = 0.f;   // [0, 1] gain envelope -- drives pre/post gains
    float bloomBasePre         = 0.f;   // dB (base offset param)
    float bloomBasePost        = 0.f;   // dB (base offset param)
    float bloomPreGainApplied  = 0.f;   // dB -- actual gain set on bloomPreGain stage this block
    float bloomPostGainApplied = 0.f;   // dB -- actual gain set on bloomPostGain stage this block
    float bloomDepth           = 0.f;   // dB
    float bloomCompensation  = 0.f; // ratio
    float bloomSensitivity   = 0.f; // dB
    float bloomAttack        = 0.f; // ms
    float bloomRelease       = 0.f; // ms
    float harmonicActivity   = 0.f; // EMA of |delta(smoothedDet)|
    int   bloomMode          = 0;   // 0 = Shaped, 1 = Tracking, 2 = Adaptive

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

    // For observation-only meters: provides the raw value for display.
    // - When observationRange is absent: getter returns a value already in [0, 1]
    //   (used for envelope [0,1], gate gain [0,1]).
    // - When observationRange is set: getter returns the raw value in physical units
    //   (e.g. dB); normalization is done using the range for bar height, and the
    //   raw value is shown as the numeric readout.
    // Ignored when paramId != kCount.
    std::function<float(const MeterData&)> valueGetter;

    // Optional explicit range for observation meters whose getter returns a
    // physical value (not already [0, 1]).
    struct ObservationRange { float min; float max; };
    std::optional<ObservationRange> observationRange;

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

    /** Create a read-only observation meter whose getter returns [0, 1]. */
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

    /**
     * Create a read-only observation meter whose getter returns a raw physical
     * value (e.g. dB). The bar is normalized using [rangeMin, rangeMax].
     * The numeric readout shows the raw value + unit.
     */
    static MeterDesc fromObservationRanged(const char* label,
                                           std::function<float(const MeterData&)> getter,
                                           float rangeMin, float rangeMax,
                                           const char* unit = "") {
        MeterDesc d;
        d.label             = label;
        d.paramId           = ParamId::kCount;
        d.valueGetter       = std::move(getter);
        d.observationRange  = ObservationRange{ rangeMin, rangeMax };
        d.alwaysReadOnly    = true;
        d.unit              = unit;
        return d;
    }

    bool isRegistryParam() const { return paramId != ParamId::kCount; }
};

} // namespace hexcaster::tui
