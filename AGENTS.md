# AGENTS.md

# HexCaster – Neural Dynamic Gain Amp Platform

---

# 1. Project Overview

HexCaster is a Linux-first, Raspberry Pi–targeted neural guitar amplifier platform.

It consists of:

* A real-time safe C++ DSP core
* Neural Amp Modeling (NAM-compatible inference)
* Dynamic pre/post gain control ("Bloom" system)
* IR convolution cabinet simulation
* Algorithmic reverb
* Parametric EQ
* Headless runtime for Raspberry Pi
* LV2 wrapper for use inside Reaper (Linux)

The LV2 plugin is for development and validation only.
The standalone runtime is the primary deployment target.

---

# 2. Design Philosophy

HexCaster is DSP-core first, not plugin-first.

The DSP library:

* Must be framework-independent
* Must be real-time safe
* Must not depend on JUCE or large frameworks
* Must not allocate memory during processing
* Must be deterministic

Wrappers (LV2, standalone host) are thin layers over the DSP core.

---

# 3. High-Level Audio Architecture

HexCaster supports two primary runtime modes:

1. Cab Mode (driving a real guitar speaker cabinet)
2. Direct Mode (headphones / FRFR / recording output)

---

## 3.1 Cab Mode (Physical Guitar Cabinet)

Signal Flow:

Input
↓
Detector HPF (for envelope only)
↓
Envelope Follower
↓
Pre-Gain Modulation
↓
Neural Amp Model (NAM)
↓
Post-Gain Compensation
↓
Post EQ (amp tone shaping)
↓
Output → Power Amp → Guitar Cab

Notes:

* No IR convolution stage in Cab Mode.
* The physical cabinet provides the speaker filtering.
* A high-cut may optionally be provided for fizz control.

---

## 3.2 Direct Mode (Headphones / Recording)

Signal Flow:

Input
↓
Detector HPF (for envelope only)
↓
Envelope Follower
↓
Pre-Gain Modulation
↓
Neural Amp Model (NAM)
↓
IR Convolution (Cab Simulation)
↓
Reverb
↓
Post EQ
↓
Post-Gain Compensation
↓
Output

---

The envelope follower runs once and drives both:

* Pre-gain (negative direction)
* Post-gain (partial positive compensation)

This guarantees synchronization and prevents envelope mismatch artifacts.

---

# 4. Core DSP Modules

## 4.1 Envelope Follower

Requirements:

* Peak-based detection
* Configurable attack and release
* Optional lookahead (1–3 ms buffer)
* Output normalized to [0.0, 1.0]
* No dynamic allocation
* No STL resizing in audio thread

Detector Pre-Filtering (applies ONLY to envelope path, not audio path):

* High-pass filter (recommended 70–150 Hz)
* Optional low-pass filter (e.g., 4–8 kHz) to reduce pick-noise spikes

Purpose:

* Prevent low-frequency thumps from dominating envelope
* Improve transient detection stability
* Make envelope respond to musical energy rather than sub content

Envelope output drives:

PreGain_dB  = BasePre  - A * envelope
PostGain_dB = BasePost + B * envelope

Both must be clamped to safe limits.

---

## 4.2 Gain Stage

Requirements:

* Internal representation in linear gain
* Parameter input in dB
* Smoothed transitions
* Hard clamp on min/max gain
* No -inf behavior
* No denormals

---

## 4.3 Neural Amp Model Integration

HexCaster uses the **NeuralAudio** library as the primary inference engine for loading and running NAM-compatible models.

NeuralAudio references:

* README: [https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md)
* Repository (SSH): [git@github.com](mailto:git@github.com):mikeoliphant/NeuralAudio.git
* Repository (HTTPS): [https://github.com/mikeoliphant/NeuralAudio](https://github.com/mikeoliphant/NeuralAudio)

NeuralAudio supports:

* NAM WaveNet models (.nam files)
* NAM LSTM models
* RTNeural keras LSTM/GRU models

Preferred configuration:

* Use NeuralAudio internal implementation (default)
* Enable static RTNeural builds for optimized architectures
* Avoid NAM Core fallback unless benchmarking

CMake recommendation:

-DBUILD_STATIC_RTNEURAL=ON

Model loading requirements:

* Model loaded at initialization only
* No allocation or model changes inside process()
* Maximum buffer size set during initialization
* Reject unsupported or overly heavy models at load time

Performance guidance for Raspberry Pi 5:

* Prefer Nano / Feather / Lite WaveNet models
* Prefer small LSTM models (1x8, 1x12)
* Benchmark Standard models before allowing in production

The DSP core must treat NeuralAudio as a deterministic, real-time-safe processing unit:

model->Process(inputPtr, outputPtr, numSamples);

Inference must complete within the audio buffer deadline.

---

## 4.4 IR Convolution

Implementation:

* Partitioned convolution (uniform partition acceptable initially)
* Pre-allocated buffers
* Fixed maximum IR length
* No dynamic allocation during processing

---

## 4.5 Reverb

Initial implementation:

* Lightweight algorithmic reverb (Freeverb-style acceptable)
* CPU efficient
* Deterministic
* No dynamic memory

---

## 4.6 Parametric EQ

Two conceptual EQ stages may exist:

1. Pre-Distortion EQ (Input Shaping)
2. Post-Distortion EQ (Tone Shaping)

### Pre-Distortion EQ (Optional, Minimal Initially)

Purpose:

* Tighten low end before distortion
* Shape how NAM saturates
* Control harshness generation

Initial implementation may include:

* High-pass filter
* Optional low-shelf

### Post-Distortion EQ (Primary Tone Control)

* 3–5 band biquad implementation
* Pre-computed coefficients
* Coefficient smoothing
* Stable at extreme parameter values

This EQ acts as the "amp tone stack" control layer.

---

# 5. Real-Time Safety Rules (Strict)

Inside process():

* No malloc/new
* No std::vector resize
* No locks
* No file I/O
* No logging
* No printf
* No blocking calls
* No exceptions

Denormals must be prevented (flush-to-zero or tiny DC offset).

Audio thread must be:

* Lock-free
* Deterministic
* Bounded-time

---

# 6. Repository Layout

hexcaster/
│
├── CMakeLists.txt
│
├── dsp/
│   ├── include/
│   │   ├── envelope.h
│   │   ├── gain_stage.h
│   │   ├── ir_convolver.h
│   │   ├── reverb.h
│   │   ├── eq.h
│   │   └── hexcaster_processor.h
│   │
│   └── src/
│       ├── envelope.cpp
│       ├── gain_stage.cpp
│       ├── ir_convolver.cpp
│       ├── reverb.cpp
│       ├── eq.cpp
│       └── hexcaster_processor.cpp
│
├── lv2/
│   ├── manifest.ttl
│   ├── hexcaster.ttl
│   └── hexcaster_lv2.cpp
│
├── standalone/
│   ├── main.cpp
│   └── audio_engine.cpp
│
└── external/
└── (RTNeural or NAM runtime)

---

# 7. Build System (CMake)

Targets:

* hexcaster_dsp (static or shared library)
* hexcaster_lv2 (MODULE, links dsp)
* hexcaster_standalone (executable, links dsp)

High-level CMake structure:

add_library(hexcaster_dsp STATIC
dsp/src/envelope.cpp
dsp/src/gain_stage.cpp
dsp/src/ir_convolver.cpp
dsp/src/reverb.cpp
dsp/src/eq.cpp
dsp/src/hexcaster_processor.cpp
)

target_include_directories(hexcaster_dsp PUBLIC dsp/include)

add_library(hexcaster_lv2 MODULE
lv2/hexcaster_lv2.cpp
)

target_link_libraries(hexcaster_lv2 PRIVATE hexcaster_dsp)

add_executable(hexcaster_standalone
standalone/main.cpp
)

target_link_libraries(hexcaster_standalone PRIVATE hexcaster_dsp)

LV2 bundle should install to:

~/.lv2/hexcaster.lv2/

---

# 8. Parameter Architecture

* Parameters stored as std::atomic<float>
* Smoothed inside DSP
* Control thread writes only
* Audio thread reads only
* No locks

Future expansion:

* Dominance metric input
* MIDI CC mapping
* External hardware control

---

# 9. Development Phases

Phase 1:

* Envelope + Pre/Post gain
* Basic EQ
* IR convolution

Phase 2:

* NAM integration
* Performance profiling on Pi

Phase 3:

* Reverb refinement
* Dominance-linked envelope control

Phase 4:

* Hardware integration
* MIDI routing
* Preset system

---

# 10. Performance Targets (Raspberry Pi 5)

* 48 kHz
* 64–128 sample buffers
* < 50% CPU total
* < 5 ms system latency

---

# 11. Long-Term Direction

HexCaster is an embedded DSP engine.

It may eventually support:

* Multi-model switching
* Per-string dominance control
* External control surfaces
* Head unit UI
* Preset storage

But the core must remain:

* Minimal
* Portable
* Real-time safe
* Independent of large frameworks

---

---

# 12. External Dependencies

The following external libraries and systems are used by HexCaster:

## 12.1 NeuralAudio (Primary Inference Engine)

Purpose:

* Load and run NAM-compatible models in real-time

Repository:

* SSH: [git@github.com](mailto:git@github.com):mikeoliphant/NeuralAudio.git
* HTTPS: [https://github.com/mikeoliphant/NeuralAudio](https://github.com/mikeoliphant/NeuralAudio)

README:

* [https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md)

Build configuration guidance:

* Enable static RTNeural builds: -DBUILD_STATIC_RTNEURAL=ON
* Avoid enabling NAMCore unless benchmarking

NeuralAudio must be treated as an initialization-time dependency only. Model loading and configuration must not occur inside the audio processing thread.

## 12.2 Linux Audio Stack (Standalone Runtime)

Standalone runtime will interface with:

* ALSA
* JACK or PipeWire (JACK-compatible mode)

Audio backend responsibilities:

* Buffer management
* Device selection
* Real-time thread priority

DSP core must remain completely independent of the audio backend implementation.

---

End of AGENTS.md
