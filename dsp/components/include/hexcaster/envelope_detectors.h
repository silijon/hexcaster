#pragma once

// envelope_detectors.hpp (revised naming conventions)
//
// Naming philosophy:
// - Prefix functions with their domain: hpf_, lpf_, env_, rms_, frontend_
// - Use verbs that describe *what is happening* (process → step / apply / update)
// - State structs are nouns; functions operating on them are verbs
//
// Example:
//   float mag = frontend_apply(frontend, sample);
//   float env = env_peak_hold_release_step(state, mag);

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hexcaster::env
{

// -----------------------------------------------------------------------------
// Coefficient helpers
// -----------------------------------------------------------------------------

inline float coeff_from_ms(float ms, float sampleRate)
{
    if (ms <= 0.0f)
        return 0.0f;

    const float seconds = ms * 0.001f;
    return std::exp(-1.0f / (seconds * sampleRate));
}

inline std::uint32_t samples_from_ms(float ms, float sampleRate)
{
    if (ms <= 0.0f)
        return 0;

    return static_cast<std::uint32_t>(ms * 0.001f * sampleRate + 0.5f);
}

inline float clamp01(float x)
{
    return std::max(0.0f, std::min(1.0f, x));
}

inline float rectify_abs(float x) { return std::fabs(x); }
inline float rectify_square(float x) { return x * x; }

// -----------------------------------------------------------------------------
// HPF
// -----------------------------------------------------------------------------

struct OnePoleHPF
{
    float b0 { 1.0f };
    float b1 { -1.0f };
    float a1 { 0.0f };
    float x1 { 0.0f };
    float y1 { 0.0f };
};

inline OnePoleHPF hpf_make(float cutoffHz, float sampleRate)
{
    OnePoleHPF f{};
    if (cutoffHz <= 0.0f) return f;

    constexpr float PI = 3.14159265358979323846f;
    const float k = std::tan(PI * cutoffHz / sampleRate);
    const float norm = 1.0f / (1.0f + k);

    f.b0 = norm;
    f.b1 = -norm;
    f.a1 = (k - 1.0f) * norm;
    return f;
}

inline float hpf_step(OnePoleHPF& f, float x)
{
    const float y = f.b0 * x + f.b1 * f.x1 - f.a1 * f.y1;
    f.x1 = x;
    f.y1 = y;
    return y;
}

// -----------------------------------------------------------------------------
// LPF
// -----------------------------------------------------------------------------

struct OnePoleLPF
{
    float coeff { 0.0f };
    float y     { 0.0f };
};

inline OnePoleLPF lpf_make_ms(float ms, float sampleRate)
{
    OnePoleLPF f{};
    f.coeff = coeff_from_ms(ms, sampleRate);
    return f;
}

inline float lpf_step(OnePoleLPF& f, float x)
{
    f.y = f.coeff * f.y + (1.0f - f.coeff) * x;
    return f.y;
}

// -----------------------------------------------------------------------------
// Frontend
// -----------------------------------------------------------------------------

struct DetectorFrontend
{
    bool useHPF       { false };
    bool usePreSmooth { false };

    OnePoleHPF hpf{};
    OnePoleLPF pre{};

    float sensitivity { 1.0f };
};

inline float frontend_apply(DetectorFrontend& f, float x)
{
    if (f.useHPF)
        x = hpf_step(f.hpf, x);

    x = rectify_abs(x) * f.sensitivity;

    if (f.usePreSmooth)
        x = lpf_step(f.pre, x);

    return x;
}

// -----------------------------------------------------------------------------
// Peak AR
// -----------------------------------------------------------------------------

struct PeakARState
{
    float env{0.0f};
    float attackCoeff{0.0f};
    float releaseCoeff{0.0f};
};

inline PeakARState env_peak_ar_make(float attackMs, float releaseMs, float sr)
{
    return {0.0f, coeff_from_ms(attackMs, sr), coeff_from_ms(releaseMs, sr)};
}

inline float env_peak_ar_step(PeakARState& s, float x)
{
    if (x > s.env)
        s.env = s.attackCoeff * s.env + (1.0f - s.attackCoeff) * x;
    else
        s.env = s.releaseCoeff * s.env + (1.0f - s.releaseCoeff) * x;
    return s.env;
}

// -----------------------------------------------------------------------------
// Instant attack
// -----------------------------------------------------------------------------

struct PeakInstantState
{
    float env{0.0f};
    float releaseCoeff{0.0f};
};

inline PeakInstantState env_peak_instant_make(float releaseMs, float sr)
{
    return {0.0f, coeff_from_ms(releaseMs, sr)};
}

inline float env_peak_instant_step(PeakInstantState& s, float x)
{
    if (x > s.env)
        s.env = x;
    else
        s.env = s.releaseCoeff * s.env + (1.0f - s.releaseCoeff) * x;
    return s.env;
}

// -----------------------------------------------------------------------------
// Peak hold release
// -----------------------------------------------------------------------------

struct PeakHoldState
{
    float env{0.0f};
    float releaseCoeff{0.0f};
    std::uint32_t hold{0};
    std::uint32_t counter{0};
};

inline PeakHoldState env_peak_hold_make(float holdMs, float releaseMs, float sr)
{
    return {0.0f, coeff_from_ms(releaseMs, sr), samples_from_ms(holdMs, sr), 0};
}

inline float env_peak_hold_step(PeakHoldState& s, float x)
{
    if (x >= s.env)
    {
        s.env = x;
        s.counter = s.hold;
    }
    else if (s.counter > 0)
    {
        --s.counter;
    }
    else
    {
        s.env *= s.releaseCoeff;
    }
    return s.env;
}

// -----------------------------------------------------------------------------
// Peak hold with hysteresis
// -----------------------------------------------------------------------------

struct PeakHoldHystState
{
    float env{0.0f};
    float releaseCoeff{0.0f};
    float threshold{0.0f};
    std::uint32_t hold{0};
    std::uint32_t counter{0};
};

inline PeakHoldHystState env_peak_hold_hyst_make(float holdMs, float releaseMs, float thresh, float sr)
{
    return {0.0f, coeff_from_ms(releaseMs, sr), thresh, samples_from_ms(holdMs, sr), 0};
}

inline float env_peak_hold_hyst_step(PeakHoldHystState& s, float x)
{
    if (x >= s.env + s.threshold)
    {
        s.env = x;
        s.counter = s.hold;
    }
    else if (s.counter > 0)
    {
        --s.counter;
    }
    else
    {
        s.env *= s.releaseCoeff;
    }
    return s.env;
}

// -----------------------------------------------------------------------------
// Peak decay
// -----------------------------------------------------------------------------

struct PeakDecayState
{
    float env{0.0f};
    float releaseCoeff{0.0f};
};

inline PeakDecayState env_peak_decay_make(float releaseMs, float sr)
{
    return {0.0f, coeff_from_ms(releaseMs, sr)};
}

inline float env_peak_decay_step(PeakDecayState& s, float x)
{
    if (x >= s.env)
        s.env = x;
    else
        s.env *= s.releaseCoeff;
    return s.env;
}

// -----------------------------------------------------------------------------
// RMS
// -----------------------------------------------------------------------------

struct RMSState
{
    float energy{0.0f};
    float coeff{0.0f};
};

inline RMSState env_rms_make(float ms, float sr)
{
    return {0.0f, coeff_from_ms(ms, sr)};
}

inline float env_rms_step(RMSState& s, float x)
{
    const float e = rectify_square(x);
    s.energy = s.coeff * s.energy + (1.0f - s.coeff) * e;
    return std::sqrt(std::max(0.0f, s.energy));
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

inline float env_mix(float a, float b, float t)
{
    t = clamp01(t);
    return a + (b - a) * t;
}

} // namespace hexcaster::env
