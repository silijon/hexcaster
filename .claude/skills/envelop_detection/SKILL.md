# SKILL: Envelope Detection for Audio DSP (HexCaster Deep Reference)

## Purpose

This document is a deep technical reference for envelope detection in real-time audio DSP systems, with a specific focus on guitar signal processing and the HexCaster architecture.

It is designed for:
- engineers building DSP systems
- musicians tuning dynamic response
- AI agents assisting with DSP design and code generation

It complements the header file `envelope_detectors.h`.

This document has four jobs:
1. define the terminology precisely
2. explain the detector techniques and why they behave differently
3. show how each concept maps to the code in `envelope_detectors.h`
4. provide practical examples for integrating the detectors into a real signal path

---

# 1. Terminology

## Audio waveform
A rapidly varying bipolar signal, usually centered around zero, representing the sound itself.

Example:
- a guitar note sampled digitally is a stream of values like `0.01, -0.02, 0.05, -0.04...`
- these sample values oscillate quickly and are not directly suitable as a stable control signal

## Control signal
A signal used to control some other process.

Examples:
- controlling gain reduction
- driving a clean/dirty blend
- driving a dynamic input gain stage
- estimating transient intensity

An envelope is a control signal.

## Envelope
A slowly varying signal that represents the changing strength, energy, or intensity of a faster audio waveform over time.

Important: the envelope is not the audio itself. It is a summary or interpretation of the audio.

## Envelope detector
An algorithm that converts a fast bipolar signal into a slower positive control signal.

## Sidechain
A signal path used only for analysis or control. It is not directly heard.

Example:
- the main audio path may go to a NAM model or amp stage
- a copy of that same audio may go into an envelope detector sidechain
- the envelope detector output then controls gain, blending, or dynamics

## Frontend
In this document and in the header file, **frontend** means the small block of detector preparation logic that happens before the main detector algorithm.

In `envelope_detectors.h`, this is represented by:
- `DetectorFrontend`
- `frontend_apply(...)`

The frontend typically does some or all of the following:
1. optional high-pass filtering of the detector sidechain
2. rectification to convert bipolar audio into positive magnitude
3. optional tiny pre-smoothing
4. sensitivity scaling

So when you see `frontend_apply(frontend, sample)`, it means:

> “Take the raw audio sample, condition it for detection, and return a detector-ready magnitude-like value.”

The frontend is not the envelope detector itself. It is the preparation stage before the detector.

## Rectification
Conversion of a bipolar signal into a unipolar signal.

### Absolute-value rectification
`abs(x)`

This is also called full-wave rectification in this context.

Purpose:
- makes negative and positive swings contribute equally to the detector
- produces a peak-sensitive magnitude signal

### Square-law rectification
`x * x`

Purpose:
- turns the signal into energy
- useful for RMS-style detectors

## Attack
How quickly a detector rises when the input increases.

Fast attack:
- catches transients quickly
- can feel sharp and responsive

Slow attack:
- smooths over spikes
- can miss the earliest part of the transient

## Release
How quickly a detector falls when the input decreases.

Fast release:
- returns quickly
- may chatter or wobble

Slow release:
- stable and smooth
- may feel sluggish or sticky

## Hold
A period during which the detector output is intentionally frozen after a peak.

Purpose:
- prevents the detector from immediately decaying after catching a transient
- reduces wobble right after the peak

## Hysteresis
A threshold rule that prevents tiny changes from retriggering the detector.

Purpose:
- ignores small bumps
- useful for chords where many partials create local mini-peaks

## Wobble
Small oscillations in the detector output during decay.

Cause:
- the detector continues to react to waveform detail after the main transient

This is especially common when the detector release still follows the incoming rectified waveform.

## Transient
The initial high-energy portion of a sound.

For guitar, this often includes:
- pick attack
- initial string excitation
- high-frequency burst at note onset

## Sustain
The later, more stable portion of a sound after the transient.

## Body
A less formal but useful term for the more sustained, tonally meaningful part of the note after the initial transient.

## Peak detector
A detector designed to react strongly to instantaneous or near-instantaneous signal peaks.

## RMS detector
A detector designed to react to average signal energy over time rather than instantaneous peaks.

## One-pole filter
A very simple smoothing filter often used in detector logic.

Canonical form:

```cpp
id="1pole"
y = a * y + (1 - a) * x;
```

Interpretation:
- `x` is the new input
- `y` is the smoothed output/state
- `a` controls how fast or slow the output moves

## Coefficient
A numeric value that controls filter behavior.

In the one-pole case:
- coefficient near `0` means very fast movement
- coefficient near `1` means very slow movement

---

# 2. Big Picture: What an Envelope Detector Is Doing

An envelope detector answers a question like:

> How strong is the signal right now, in a way that is useful for control?

This is why envelope detection is not just measurement. It is interpretation.

Two different detectors can look at the same guitar note and produce very different control signals:
- one may emphasize the pick attack
- one may emphasize overall body
- one may wobble heavily on chords
- one may remain stable and monotonic

The “best” detector depends on the control problem.

---

# 3. Canonical Detector Pipeline

A common detector pipeline is:

```text
raw audio sample
    -> optional sidechain HPF
    -> rectification
    -> optional tiny pre-smoothing
    -> detector algorithm
    -> envelope / control output
```

In the header file, that often looks like:

```cpp
id="pipeline_example"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(120.0f, sampleRate);
frontend.usePreSmooth = true;
frontend.pre = lpf_make_ms(0.25f, sampleRate);
frontend.sensitivity = 1.0f;

PeakHoldState detector = env_peak_hold_make(1.0f, 30.0f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float detectorInput = frontend_apply(frontend, audio[i]);
    const float envelope = env_peak_hold_step(detector, detectorInput);

    control[i] = envelope;
}
```

Explanation:
- `frontend_apply(...)` prepares the raw sample for detection
- `env_peak_hold_step(...)` updates the chosen detector state
- `envelope` is the control signal you can use elsewhere

---

# 4. Code Building Blocks from the Header

## 4.1 Rectification helpers

### `rectify_abs(float x)`
Returns `fabs(x)`.

Use when:
- you want a peak-oriented magnitude signal
- you are building a peak detector

Example:

```cpp
id="rectify_abs_example"
const float sidechainSample = -0.37f;
const float magnitude = hexcaster::env::rectify_abs(sidechainSample);
// magnitude = 0.37f
```

### `rectify_square(float x)`
Returns `x * x`.

Use when:
- you want an energy measure
- you are building RMS logic

Example:

```cpp
id="rectify_square_example"
const float sidechainSample = -0.37f;
const float energy = hexcaster::env::rectify_square(sidechainSample);
// energy = 0.1369f
```

---

## 4.2 High-pass filter helpers

### `OnePoleHPF`
State structure for the sidechain high-pass filter.

Fields:
- coefficients: `b0`, `b1`, `a1`
- previous input/output samples: `x1`, `y1`

### `hpf_make(float cutoffHz, float sampleRate)`
Constructs a one-pole HPF state with coefficients initialized.

### `hpf_step(OnePoleHPF& f, float x)`
Processes one sample through the HPF and updates filter state.

Use when:
- you want the detector to respond less to low-end bulk
- you want more emphasis on pick attack or motion

Example:

```cpp
id="hpf_example"
using namespace hexcaster::env;

OnePoleHPF hpf = hpf_make(120.0f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float filtered = hpf_step(hpf, audio[i]);
    sidechain[i] = filtered;
}
```

Interpretation:
- a 120 Hz HPF in the detector sidechain makes the detector less driven by low-frequency excursions
- this can make transient control feel tighter on guitar

---

## 4.3 Low-pass filter helpers

### `OnePoleLPF`
State structure for a simple smoother.

Fields:
- `coeff`: smoothing coefficient
- `y`: last output/sample of the smoother

### `lpf_make_ms(float ms, float sampleRate)`
Constructs a one-pole LPF with coefficient derived from a time constant in milliseconds.

### `lpf_step(OnePoleLPF& f, float x)`
Processes one sample through the LPF and updates the filter state.

Use when:
- you want tiny pre-smoothing before detection
- you want a slower secondary control envelope
- you want generic smoothing of a control signal

Example:

```cpp
id="lpf_example"
using namespace hexcaster::env;

OnePoleLPF pre = lpf_make_ms(0.25f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float magnitude = rectify_abs(audio[i]);
    const float smoothedMagnitude = lpf_step(pre, magnitude);
    detectorInput[i] = smoothedMagnitude;
}
```

Important note:
- very small pre-smoothing often works better for transient control than putting a larger LPF after the detector output

---

## 4.4 Frontend helpers

### `DetectorFrontend`
A convenience structure that groups together the common detector preparation stages.

Fields:
- `useHPF`: whether to apply sidechain HPF
- `usePreSmooth`: whether to apply pre-smoothing
- `hpf`: the HPF state
- `pre`: the LPF state used for pre-smoothing
- `sensitivity`: scalar multiplier applied after rectification

### `frontend_apply(DetectorFrontend& f, float x)`
Applies the configured frontend to one raw audio sample and returns a detector-ready value.

What it does internally:
1. optional `hpf_step(...)`
2. `rectify_abs(...)`
3. multiply by `sensitivity`
4. optional `lpf_step(...)`

Example:

```cpp
id="frontend_example"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(100.0f, sampleRate);
frontend.usePreSmooth = true;
frontend.pre = lpf_make_ms(0.2f, sampleRate);
frontend.sensitivity = 1.5f;

for (int i = 0; i < numSamples; ++i) {
    const float detectorInput = frontend_apply(frontend, audio[i]);
    prepared[i] = detectorInput;
}
```

What `sensitivity` means here:
- it scales the detector input, not the audio output
- useful when you want the detector to respond more or less aggressively to the same signal

---

# 5. Detector Techniques

## 5.1 Classic Peak Attack/Release Detector

### What it is
A detector that rises using one time constant and falls using another, while continuously following the current input.

### Header file mapping
- state type: `PeakARState`
- constructor: `env_peak_ar_make(...)`
- update step: `env_peak_ar_step(...)`

### Core behavior
If the current detector input is above the current envelope, apply attack smoothing. Otherwise, apply release smoothing.

Conceptually:

```cpp
if (x > env)
    env = attack_smooth(env, x);
else
    env = release_smooth(env, x);
```

### Why it is useful
This is the canonical “standard” envelope follower. It is easy to understand and often good enough for general-purpose use.

### Why it wobbles
Because even during release, it still pays attention to the incoming rectified waveform. If that waveform ripples, the detector output ripples.

### Example

```cpp
id="peak_ar_example"
using namespace hexcaster::env;

PeakARState detector = env_peak_ar_make(
    1.0f,   // attack ms
    30.0f,  // release ms
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float x = frontend_apply(frontend, audio[i]);
    const float env = env_peak_ar_step(detector, x);
    control[i] = env;
}
```

### When to use it
Use this when:
- you want a standard envelope follower
- you are exploring baseline behavior
- you do not yet need the cleanest transient-only behavior

### Trade-offs
Pros:
- familiar and intuitive
- flexible
- musically reasonable in many cases

Cons:
- falling edge can wobble
- not ideal for the sharpest transient control

---

## 5.2 Instant-Attack Peak Detector

### What it is
A detector that jumps immediately to new peaks but still uses a smoothed release.

### Header file mapping
- state type: `PeakInstantState`
- constructor: `env_peak_instant_make(...)`
- update step: `env_peak_instant_step(...)`

### Why it exists
When transient timing matters, even a small attack smoother can blunt the earliest part of the transient. This detector fixes that by making attack effectively instantaneous.

### Core behavior
- if `x > env`, set `env = x`
- else, release smoothly toward `x`

### Example

```cpp
id="peak_instant_example"
using namespace hexcaster::env;

PeakInstantState detector = env_peak_instant_make(
    25.0f,  // release ms
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float x = frontend_apply(frontend, audio[i]);
    const float env = env_peak_instant_step(detector, x);
    control[i] = env;
}
```

### When to use it
Use this when:
- you care strongly about catching the very beginning of the pick transient
- you want a simpler alternative before moving to hold-based detectors

### Trade-offs
Pros:
- excellent transient timing
- very simple

Cons:
- release still follows waveform detail
- wobble can remain during decay

---

## 5.3 Peak-Hold + Release Detector

### What it is
A detector that catches peaks immediately, holds them briefly, then decays independently of the incoming waveform.

### Header file mapping
- state type: `PeakHoldState`
- constructor: `env_peak_hold_make(...)`
- update step: `env_peak_hold_step(...)`

### Why it is often the best choice for guitar transient control
The hold stage stops the detector from immediately dropping after the transient. The independent release avoids most waveform-coupled wobble.

This is often the best “sharp but stable” detector topology.

### Core behavior
- if a new input exceeds the envelope, jump to it and reload hold timer
- while hold is active, keep the envelope frozen
- after hold expires, decay the envelope exponentially toward zero

### Example

```cpp
id="peak_hold_example"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(120.0f, sampleRate);
frontend.usePreSmooth = false;
frontend.sensitivity = 1.0f;

PeakHoldState detector = env_peak_hold_make(
    1.0f,   // hold ms
    30.0f,  // release ms
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float x = frontend_apply(frontend, audio[i]);
    const float env = env_peak_hold_step(detector, x);
    control[i] = env;
}
```

### Interpretation of the parameters
- `hold ms`: how long to freeze after a peak
- `release ms`: how quickly to decay after the hold ends

### Typical starting values for guitar
- hold: `0.5` to `1.5` ms
- release: `15` to `40` ms

### When to use it
Use this when:
- you want strong transient capture
- you want a clean falling edge
- you are fighting wobble on single notes or chords

### Trade-offs
Pros:
- catches transients well
- much less wobble
- strong default choice for HexCaster-like control

Cons:
- too much hold can feel sticky
- repeated very fast picking may blur together if hold is too long

---

## 5.4 Peak-Hold + Hysteresis Detector

### What it is
A peak-hold detector that only retriggers when the new input clearly exceeds the current envelope by a threshold.

### Header file mapping
- state type: `PeakHoldHystState`
- constructor: `env_peak_hold_hyst_make(...)`
- update step: `env_peak_hold_hyst_step(...)`

### Why it exists
With chords, the rectified sidechain often contains many small local peaks after the main transient. Hysteresis prevents those tiny fluctuations from re-triggering the envelope unnecessarily.

### Core behavior
A new peak only counts if:

```cpp
new_input >= current_envelope + threshold
```

### Example

```cpp
id="peak_hold_hyst_example"
using namespace hexcaster::env;

PeakHoldHystState detector = env_peak_hold_hyst_make(
    1.0f,    // hold ms
    30.0f,   // release ms
    0.01f,   // hysteresis threshold in detector units
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float x = frontend_apply(frontend, audio[i]);
    const float env = env_peak_hold_hyst_step(detector, x);
    control[i] = env;
}
```

### What the threshold means
It is measured in the same units as the detector input and envelope.

Larger threshold:
- more stable
- less sensitive to subtle changes

Smaller threshold:
- more responsive
- may allow more retriggers

### When to use it
Use this when:
- chords cause visible wobble
- the detector keeps hopping upward from small bumps
- you want a more conservative retrigger rule

### Trade-offs
Pros:
- excellent chord stability
- reduces micro retriggers

Cons:
- can ignore subtle dynamic events if threshold is too high

---

## 5.5 Pure Peak Decay Detector

### What it is
A minimal detector that jumps instantly to new peaks and otherwise decays toward zero.

### Header file mapping
- state type: `PeakDecayState`
- constructor: `env_peak_decay_make(...)`
- update step: `env_peak_decay_step(...)`

### Why it matters
This is one of the cleanest ways to decouple the release behavior from the incoming waveform. It is useful as both a practical detector and a diagnostic baseline.

### Core behavior
- if `x >= env`, set `env = x`
- else, `env *= releaseCoeff`

### Example

```cpp
id="peak_decay_example"
using namespace hexcaster::env;

PeakDecayState detector = env_peak_decay_make(
    20.0f,  // release ms
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float x = frontend_apply(frontend, audio[i]);
    const float env = env_peak_decay_step(detector, x);
    control[i] = env;
}
```

### When to use it
Use this when:
- you want the smallest and cleanest peak tracker
- you want to test how much wobble is caused by waveform-following release
- you want a low-complexity transient detector

### Trade-offs
Pros:
- very clean decay
- no release coupling to waveform
- easy to reason about

Cons:
- can decay too quickly right after the peak without a hold stage
- may feel a bit too “needle-like” in some musical contexts

---

## 5.6 RMS Detector

### What it is
A detector that tracks average energy rather than instantaneous peak level.

### Header file mapping
- state type: `RMSState`
- constructor: `env_rms_make(...)`
- update step: `env_rms_step(...)`

### Why it behaves differently
Instead of rectifying with `abs(x)` and following peaks, RMS detection squares the signal, smooths the energy, then takes the square root.

This is closer to tracking overall level than transient peaks.

### Important input note
`env_rms_step(...)` is intended to operate on a detector sample directly. Often that means a sidechain-filtered sample, not an already rectified magnitude.

### Example

```cpp
id="rms_example"
using namespace hexcaster::env;

OnePoleHPF hpf = hpf_make(80.0f, sampleRate);
RMSState detector = env_rms_make(
    20.0f,  // averaging ms
    sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float sidechain = hpf_step(hpf, audio[i]);
    const float env = env_rms_step(detector, sidechain);
    control[i] = env;
}
```

### When to use it
Use this when:
- you care about average energy or loudness-like behavior
- you want a slower, calmer control signal
- you are building level-dependent logic rather than sharp transient logic

### Trade-offs
Pros:
- smooth and stable
- less spiky than peak detection

Cons:
- slower
- less useful for precise transient capture

---

# 6. Wobble: Detailed Explanation

## What wobble really is
Wobble is the detector output moving up and down during the supposed decay phase because it is still reacting to local detail in the rectified input.

## Why rectification does not solve everything
Rectification turns negative values positive, but it does not remove oscillation. It only changes the polarity behavior.

For example, a rectified guitar waveform can still have many local humps caused by:
- note frequency
- harmonics
- pick noise remnants
- interference between strings in chords

## Why chords wobble more
A chord contains multiple fundamentals and harmonics. Those components interact, producing amplitude modulation and beating. The rectified sidechain therefore becomes more irregular and “lumpy,” which makes a classic release-following detector wobble more.

## Typical cures, in order of usefulness
1. instantaneous attack
2. short hold
3. release that decays independently of input
4. hysteresis
5. tiny pre-smoothing

---

# 7. Practical Frontend and Detector Combinations

## 7.1 Sharp transient detector for guitar

Goal:
- catch pick onset quickly
- minimize wobble

Suggested combination:
- `DetectorFrontend`
- `hpf_make(...)`
- `frontend_apply(...)`
- `env_peak_hold_step(...)`

Example:

```cpp
id="combo_sharp_guitar"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(120.0f, sampleRate);
frontend.usePreSmooth = false;
frontend.sensitivity = 1.0f;

PeakHoldState detector = env_peak_hold_make(0.75f, 25.0f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float detectorInput = frontend_apply(frontend, audio[i]);
    const float env = env_peak_hold_step(detector, detectorInput);
    transientControl[i] = env;
}
```

## 7.2 Stable chord detector

Goal:
- suppress chord-induced retrigger chatter

Suggested combination:
- `DetectorFrontend`
- `env_peak_hold_hyst_step(...)`

Example:

```cpp
id="combo_stable_chords"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(100.0f, sampleRate);
frontend.usePreSmooth = true;
frontend.pre = lpf_make_ms(0.2f, sampleRate);
frontend.sensitivity = 1.0f;

PeakHoldHystState detector = env_peak_hold_hyst_make(1.0f, 30.0f, 0.01f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float detectorInput = frontend_apply(frontend, audio[i]);
    const float env = env_peak_hold_hyst_step(detector, detectorInput);
    chordControl[i] = env;
}
```

## 7.3 Slow body / sustain detector

Goal:
- track longer-term intensity rather than sharp transient detail

Suggested combination:
- sidechain HPF optional
- `env_rms_step(...)` or `lpf_step(...)`

Example using RMS:

```cpp
id="combo_body_rms"
using namespace hexcaster::env;

OnePoleHPF hpf = hpf_make(60.0f, sampleRate);
RMSState detector = env_rms_make(30.0f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float sidechain = hpf_step(hpf, audio[i]);
    const float env = env_rms_step(detector, sidechain);
    sustainControl[i] = env;
}
```

---

# 8. Parameter Tuning Guide

## Hold time
What it controls:
- how long the detected peak is frozen before decay begins

Typical interpretation:
- `0 ms`: no hold, more jitter or wobble right after the peak
- `0.5 ms`: catches the transient cleanly, little added stickiness
- `1.0–1.5 ms`: often a sweet spot for guitar
- `> 3 ms`: can feel sticky or blur rapid picking

## Release time
What it controls:
- how long the envelope takes to fall after the peak or hold stage

Typical interpretation:
- `< 10 ms`: very fast, possibly twitchy
- `15–40 ms`: often musical and usable for guitar transient control
- `> 80 ms`: calm but potentially sluggish

## HPF cutoff
What it controls:
- how much low-frequency content influences the detector

Typical interpretation:
- lower cutoff: more body, more low-end influence
- higher cutoff: more attack emphasis, less bass-driven triggering

## Pre-smoothing time
What it controls:
- how much tiny ripple is removed before the detector sees the sidechain

Typical interpretation:
- `0 ms`: most direct and sharp
- `0.1–0.3 ms`: a subtle stabilizer
- larger values: begins to blur transient timing

## Hysteresis threshold
What it controls:
- how much larger a new peak must be before it retriggers the detector

Typical interpretation:
- very small threshold: more responsive, less stable
- larger threshold: more stable, less sensitive to subtle dynamics

---

# 9. Symptom -> Likely Cause -> Likely Fix

## Symptom: detector peak feels late
Likely cause:
- attack smoothing is too slow
- too much pre-smoothing

Likely fixes:
- use `env_peak_instant_step(...)`
- move to `env_peak_hold_step(...)`
- reduce or remove `usePreSmooth`

## Symptom: falling edge wobbles badly
Likely cause:
- release is still following waveform detail

Likely fixes:
- use `env_peak_decay_step(...)`
- use `env_peak_hold_step(...)`
- add mild frontend pre-smoothing

## Symptom: chords retrigger repeatedly
Likely cause:
- local peaks in rectified chord signal exceed envelope repeatedly

Likely fixes:
- use `env_peak_hold_hyst_step(...)`
- increase hold slightly
- tune HPF cutoff

## Symptom: detector feels sticky
Likely cause:
- hold too long
- release too slow

Likely fixes:
- shorten hold time
- reduce release time

## Symptom: detector ignores subtle dynamics
Likely cause:
- hysteresis too large
- HPF cutoff too high

Likely fixes:
- reduce hysteresis threshold
- lower sidechain HPF cutoff

---

# 10. HexCaster-Oriented Guidance

For the current HexCaster use case, the main design goal is usually not “measure average loudness.” It is more like:

> detect the immediate pick transient and generate a control signal that is sharp, stable, and musically useful for dynamic gain control

That makes the following stack a strong default:

```cpp
id="hexcaster_default"
using namespace hexcaster::env;

DetectorFrontend frontend{};
frontend.useHPF = true;
frontend.hpf = hpf_make(120.0f, sampleRate);
frontend.usePreSmooth = false;
frontend.sensitivity = 1.0f;

PeakHoldState detector = env_peak_hold_make(1.0f, 30.0f, sampleRate);

for (int i = 0; i < numSamples; ++i) {
    const float detectorInput = frontend_apply(frontend, inputBuffer[i]);
    const float transientEnv = env_peak_hold_step(detector, detectorInput);

    // Example: use transientEnv to drive a gain-control law.
    controlBuffer[i] = transientEnv;
}
```

If chord wobble remains a problem, try:

```cpp
id="hexcaster_chord_default"
PeakHoldHystState detector = env_peak_hold_hyst_make(1.0f, 30.0f, 0.01f, sampleRate);
```

---

# 11. Final Recommendations

## Best baseline detector for HexCaster-style transient control
Use:
- `DetectorFrontend`
- `frontend_apply(...)`
- `env_peak_hold_step(...)`

## Best fallback if chords remain unstable
Use:
- `env_peak_hold_hyst_step(...)`

## Best diagnostic detector when studying wobble
Use:
- `env_peak_decay_step(...)`

## Best detector for average energy or body tracking
Use:
- `env_rms_step(...)`

---

# 12. Quick Reference Table

| Goal | Suggested functions |
|---|---|
| Prepare raw samples for detection | `DetectorFrontend`, `frontend_apply`, `hpf_make`, `hpf_step`, `lpf_make_ms`, `lpf_step` |
| Standard envelope follower | `env_peak_ar_make`, `env_peak_ar_step` |
| Fast transient detection | `env_peak_instant_make`, `env_peak_instant_step` |
| Sharp + stable transient control | `env_peak_hold_make`, `env_peak_hold_step` |
| Chord-resistant transient control | `env_peak_hold_hyst_make`, `env_peak_hold_hyst_step` |
| Minimal pure-decay peak tracking | `env_peak_decay_make`, `env_peak_decay_step` |
| Average energy tracking | `env_rms_make`, `env_rms_step` |
| Blend two control signals | `env_mix` |

---

End of SKILL.md

