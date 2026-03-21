# AGENTS.md

# HexCaster – Neural Dynamic Gain Amp Platform

______________________________________________________________________

# 1. Project Overview

HexCaster is a Linux-first, Raspberry Pi–targeted neural guitar amplifier platform.

It consists of:

- A real-time safe C++ DSP core
- Neural Amp Modeling (NAM-compatible inference)
- Dynamic pre/post gain control ("Bloom" system)
- Headless runtime for Raspberry Pi
- LV2 wrapper for use inside Reaper (Linux)

The LV2 plugin is for development and validation only.
The standalone runtime is the primary deployment target.

______________________________________________________________________

# 2. Design Philosophy

HexCaster is DSP-core first, not plugin-first.

The DSP library:

- Must be framework-independent
- Must be real-time safe
- Must not depend on JUCE or large frameworks
- Must not allocate memory during processing
- Must be deterministic

Wrappers (LV2, standalone host) are thin layers over the DSP core.

______________________________________________________________________

## 3.1 Cab Mode (Physical Guitar Cabinet)

Signal Flow:

Input
↓
Noise Gate (to control for noise pickups)
↓
Detector HPF (for envelope only)
↓
Envelope Follower
↓
Input Gain (fixed by external amp control -- e.g. physical knob)
↓
Pre-Gain Modulation (dynamically controlled via envelope follower)
↓
Neural Amp Model (NAM)
↓
Post-Gain Compensation (dynamically controlled via envelope follower)
↓
Post EQ (amp tone shaping)
↓
Master Volume (fixed by external amp control -- e.g. physical knob)
↓
Output → Power Amp → Guitar Cab

Notes:

- No IR convolution stage in Cab Mode.
- The physical cabinet provides the speaker filtering.
- A high-cut may optionally be provided for fizz control.

______________________________________________________________________

The envelope follower runs once and drives both:

- Pre-gain (negative direction)
- Post-gain (partial positive compensation)

This guarantees synchronization and prevents envelope mismatch artifacts.

______________________________________________________________________

# 4. Core DSP Modules

## 4.1 Envelope Follower

Requirements:

- Peak-based detection
- Configurable attack and release
- Optional lookahead (1–3 ms buffer)
- Output normalized to [0.0, 1.0]
- No dynamic allocation
- No STL resizing in audio thread

Detector Pre-Filtering (applies ONLY to envelope path, not audio path):

- High-pass filter (recommended 70–150 Hz)
- Optional low-pass filter (e.g., 4–8 kHz) to reduce pick-noise spikes

Purpose:

- Prevent low-frequency thumps from dominating envelope
- Improve transient detection stability
- Make envelope respond to musical energy rather than sub content

Envelope output drives:

reductionDb = BloomDepth * envelope
PreGain_dB  = BloomBasePre_dB  - reductionDb
PostGain_dB = BloomBasePost_dB + BloomCompensation * reductionDb

Where BloomCompensation is a ratio [0, 2.0] (default 0.5) applied to the
input reduction. At 1.0, output exactly compensates input reduction.

Both values are clamped to safe limits.

______________________________________________________________________

## 4.2 Gain Stage

Requirements:

- Internal representation in linear gain
- Parameter input in dB
- Smoothed transitions
- Hard clamp on min/max gain
- No -inf behavior
- No denormals

______________________________________________________________________

## 4.3 Neural Amp Model Integration

HexCaster uses the **NeuralAudio** library as the primary inference engine for loading and running NAM-compatible models.

NeuralAudio references:

- README: [https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md)
- Repository (SSH): [git@github.com](mailto:git@github.com):mikeoliphant/NeuralAudio.git
- Repository (HTTPS): [https://github.com/mikeoliphant/NeuralAudio](https://github.com/mikeoliphant/NeuralAudio)

NeuralAudio supports:

- NAM WaveNet models (.nam files)
- NAM LSTM models
- RTNeural keras LSTM/GRU models

Preferred configuration:

- Use NeuralAudio internal implementation (default)
- Enable static RTNeural builds for optimized architectures
- Avoid NAM Core fallback unless benchmarking

CMake recommendation:

-DBUILD_STATIC_RTNEURAL=ON

Model loading requirements:

- Model loaded at initialization only
- No allocation or model changes inside process()
- Maximum buffer size set during initialization
- Reject unsupported or overly heavy models at load time

Performance guidance for Raspberry Pi 5:

- Prefer Nano / Feather / Lite WaveNet models
- Prefer small LSTM models (1x8, 1x12)
- Benchmark Standard models before allowing in production

The DSP core must treat NeuralAudio as a deterministic, real-time-safe processing unit:

model->Process(inputPtr, outputPtr, numSamples);

Inference must complete within the audio buffer deadline.

______________________________________________________________________

## 4.6 EQ

Implemented as mid-sweep EQ:

- Gain range: ±12 dB (controllable parameter)
- Mid sweep range: 300 Hz → 2.5 kHz (controllable parameter)
- Q: ~0.8–1.0 (fixed)

______________________________________________________________________

# 5. Real-Time Safety Rules (Strict)

Inside process():

- No malloc/new
- No std::vector resize
- No locks
- No file I/O
- No logging
- No printf
- No blocking calls
- No exceptions

Denormals must be prevented (flush-to-zero or tiny DC offset).

Audio thread must be:

- Lock-free
- Deterministic
- Bounded-time

______________________________________________________________________

# 6. Repository Layout

hexcaster/
│
├── CMakeLists.txt
├── cmake/
│   └── dependencies.cmake        # FetchContent for NeuralAudio
│
├── dsp/
│   ├── CMakeLists.txt
│   ├── components/               # Individual DSP stages (hexcaster_components)
│   │   ├── include/hexcaster/
│   │   │   ├── processor_stage.h # Abstract stage interface
│   │   │   ├── gain_stage.h
│   │   │   ├── nam_stage.h       # NeuralAudio wrapper
│   │   │   ├── noise_gate.h
│   │   │   ├── eq.h              # Mid-sweep biquad peaking EQ
│   │   │   └── envelope.h        # (stub -- future standalone envelope component)
│   │   └── src/
│   │       ├── gain_stage.cpp
│   │       ├── nam_stage.cpp
│   │       ├── noise_gate.cpp
│   │       └── eq.cpp
│   │
│   └── pipeline/                 # Signal flow composition (hexcaster_pipeline)
│       ├── include/hexcaster/
│       │   ├── pipeline.h        # Ordered stage chain + controller hooks
│       │   └── bloom_controller.h # Envelope-driven pre/post gain coordinator
│       └── src/
│           ├── pipeline.cpp
│           └── bloom_controller.cpp
│
├── params/                       # Parameter system (hexcaster_params)
│   ├── CMakeLists.txt
│   ├── include/hexcaster/
│   │   ├── param_id.h            # ParamId enum + name lookup table
│   │   ├── param_registry.h      # Atomic float store
│   │   ├── param_smoother.h      # Per-sample EMA smoother
│   │   └── midi_map.h            # CC-to-ParamId mapping
│   └── src/
│       ├── param_registry.cpp
│       ├── param_smoother.cpp
│       └── midi_map.cpp
│
├── hosts/
│   ├── lv2/                      # LV2 plugin (development/validation)
│   │   ├── CMakeLists.txt
│   │   ├── manifest.ttl
│   │   ├── hexcaster.ttl
│   │   └── hexcaster_lv2.cpp
│   │
│   └── standalone/               # Headless runtime (primary deployment)
│       ├── CMakeLists.txt
│       ├── audio_engine.h        # Abstract AudioEngine interface
│       ├── alsa_audio_engine.h   # ALSA backend
│       ├── alsa_audio_engine.cpp
│       ├── midi_input.h          # ALSA raw MIDI reader
│       ├── midi_input.cpp
│       └── main.cpp
│
├── tests/
│   ├── CMakeLists.txt
│   └── test_passthrough.cpp
│
└── external/                     # Empty -- NeuralAudio fetched via FetchContent

______________________________________________________________________

# 7. Build System (CMake)

Targets:

- hexcaster_params (static lib: param registry, smoother, MIDI map)
- hexcaster_components (static lib: DSP stages, links NeuralAudio)
- hexcaster_pipeline (static lib: signal flow composition, links components + params)
- hexcaster_lv2 (MODULE: LV2 plugin, links pipeline + params)
- hexcaster_standalone (executable: ALSA runtime, links pipeline + params)
- hexcaster_tests (executable: unit tests)

Build options (all default ON):

-DHEXCASTER_BUILD_LV2=ON
-DHEXCASTER_BUILD_STANDALONE=ON
-DHEXCASTER_BUILD_TESTS=ON

NeuralAudio is fetched automatically via CMake FetchContent on first configure.
No submodules required. Pinned to a specific release commit.

LV2 bundle auto-installs to ~/.lv2/hexcaster.lv2/ after each build.
LV2 build is skipped gracefully if lv2 headers are not installed.

______________________________________________________________________

# 8. Parameter Architecture

- Parameters stored as std::atomic<float>
- Smoothed inside DSP
- Control thread writes only
- Audio thread reads only
- No locks
- MIDI CC → ParamId mapping implemented (MidiMap + MidiInput in standalone host)
- CLI: --midi-device, --midi-cc <cc>:<ParamName> (repeatable)

Future expansion:

- Dominance metric input
- External hardware control (physical knobs/pedals)
- Preset storage

______________________________________________________________________

# 9. Development Phases

Phase 1 (Done):

- Input Gain
- NAM integration
- MIDI CC control
- ALSA standalone host on Pi 5

Phase 2 (Done):

- Noise Gate
- Mid-sweep EQ
- Master Volume

Phase 3 (Done):

- Envelope follower (100 Hz detector HPF, per-sample peak tracking)
- Bloom controller (pre/post gain modulation around NAM)
- BloomDepth + BloomCompensation ratio parameters

Future:

- Dominance-linked envelope control

______________________________________________________________________

# 10. Performance Targets (Raspberry Pi 5)

- 48 kHz
- 64–128 sample buffers
- < 50% CPU total
- < 5 ms system latency

______________________________________________________________________

# 11. Long-Term Direction

HexCaster is an embedded DSP engine.

It may eventually support:

- Multi-model switching
- Per-string dominance control
- External control surfaces
- Head unit UI
- Preset storage

But the core must remain:

- Minimal
- Portable
- Real-time safe
- Independent of large frameworks

______________________________________________________________________

# 12. External Dependencies

The following external libraries and systems are used by HexCaster:

## 12.1 NeuralAudio (Primary Inference Engine)

Purpose:

- Load and run NAM-compatible models in real-time

Repository:

- SSH: [git@github.com](mailto:git@github.com):mikeoliphant/NeuralAudio.git
- HTTPS: [https://github.com/mikeoliphant/NeuralAudio](https://github.com/mikeoliphant/NeuralAudio)

README:

- [https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/refs/heads/release/README.md)

Build configuration guidance:

- Enable static RTNeural builds: -DBUILD_STATIC_RTNEURAL=ON
- Avoid enabling NAMCore unless benchmarking

NeuralAudio must be treated as an initialization-time dependency only. Model loading and configuration must not occur inside the audio processing thread.

## 12.2 Linux Audio Stack (Standalone Runtime)

Standalone runtime will interface with:

- ALSA
- JACK or PipeWire (JACK-compatible mode)

Audio backend responsibilities:

- Buffer management
- Device selection
- Real-time thread priority

DSP core must remain completely independent of the audio backend implementation.

______________________________________________________________________

End of AGENTS.md
