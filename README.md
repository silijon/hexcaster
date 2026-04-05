# HexCaster

Neural dynamic gain amp platform for Linux / Raspberry Pi / macOS.

HexCaster is a DSP-core-first guitar amplifier engine built around Neural Amp Modeling (NAM). It provides a noise gate, dynamic pre/post gain control ("Bloom"), and a mid-sweep EQ. The core is real-time safe, framework-independent, and targets embedded deployment on Raspberry Pi 5 driving a physical guitar cabinet.

Hosting wrappers (LV2, CLAP, standalone daemon) are thin layers over the DSP core. The LV2 and CLAP plugins are the primary development and validation targets; the standalone runtime is the production deployment target.

## Repository Layout

```
hexcaster/
├── dsp/
│   ├── components/     # Individual DSP stages (GainStage, NamStage, NoiseGate, EQ, ...)
│   └── pipeline/       # Signal flow composition (Pipeline, BloomController)
├── params/             # Parameter system (registry, smoothing, MIDI mapping)
├── hosts/
│   ├── clap/           # CLAP plugin wrapper (macOS ARM64 + Linux)
│   ├── lv2/            # LV2 plugin wrapper (Linux)
│   └── standalone/     # Headless JACK/ALSA runtime (Linux)
├── tests/              # Build validation and DSP unit tests
└── external/           # Dependencies (NeuralAudio fetched via CMake FetchContent)
```

## Dependencies

**Required:**
- CMake >= 3.18
- GCC (Linux) or Apple Clang (macOS) with C++20 support

**For standalone runtime (Linux only):**
- `libasound2-dev` (Debian/Ubuntu) / `alsa-lib-devel` (Fedora)

**For LV2 plugin (Linux only, optional):**
- `liblv2-dev` (Debian/Ubuntu) / `lv2-devel` (Fedora)

**For CLAP plugin:**
- No extra system packages. The CLAP SDK is fetched automatically by CMake.

## Building

### macOS (CLAP plugin for Reaper)

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHEXCASTER_BUILD_CLAP=ON \
  -DHEXCASTER_BUILD_LV2=OFF \
  -DHEXCASTER_BUILD_STANDALONE=OFF \
  -DHEXCASTER_BUILD_TUI=OFF \
  -DHEXCASTER_BUILD_TESTS=OFF

cmake --build build
```

The plugin bundle is automatically installed to `~/Library/Audio/Plug-Ins/CLAP/hexcaster.clap/` after each build. Rescan plugins in Reaper; the plugin appears as **HexCaster** under CLAP.

To generate a `compile_commands.json` for LSP/editor tooling, add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to the configure step, then symlink it to the project root:

```sh
ln -sf build/compile_commands.json compile_commands.json
```

### Linux (all targets)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Build options (all default ON):

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHEXCASTER_BUILD_LV2=ON \
  -DHEXCASTER_BUILD_CLAP=ON \
  -DHEXCASTER_BUILD_STANDALONE=ON \
  -DHEXCASTER_BUILD_TESTS=ON
```

### Build LV2 plugin (Linux)

```sh
cmake --build build --target hexcaster_lv2 -j$(nproc)
```

The plugin bundle is automatically installed to `~/.lv2/hexcaster.lv2/` after each build. No separate install step required during development.

To load in Reaper or another LV2 host, rescan plugins. The plugin appears as **HexCaster** under LV2.

### Build CLAP plugin

```sh
cmake --build build --target hexcaster_clap -j$(nproc)
```

On macOS the bundle is installed to `~/Library/Audio/Plug-Ins/CLAP/hexcaster.clap/`. On Linux the plugin is installed to `~/.clap/hexcaster.clap`.

### Build standalone runtime

```sh
cmake --build build --target hexcaster_standalone -j$(nproc)
```

Run with a NAM model:

```sh
./build/hosts/standalone/hexcaster_standalone \
  --model /path/to/model.nam \
  --device hw:2,0 \
  --buffer 128
```

Separate input and output devices:

```sh
./build/hosts/standalone/hexcaster_standalone \
  --model /path/to/model.nam \
  --input-device hw:2,0 \
  --output-device hw:3,0
```

With MIDI CC control:

```sh
./build/hosts/standalone/hexcaster_standalone \
  --model /path/to/model.nam \
  --input-device hw:CARD=V276,DEV=0 \
  --output-device hw:CARD=sndrpihifiberry,DEV=0 \
  --midi-device hw:1,0,0 \
  --midi-cc 7:InputGain_dB \
  --midi-cc 1:BloomBasePre_dB
```

List available ALSA audio devices:

```sh
./build/hosts/standalone/hexcaster_standalone --list-devices
```

List available ALSA MIDI devices:

```sh
./build/hosts/standalone/hexcaster_standalone --list-midi
```

See all options:

```sh
./build/hosts/standalone/hexcaster_standalone --help
```

### Build everything

```sh
cmake --build build -j$(nproc)
```

### Run tests

```sh
cmake --build build --target hexcaster_tests -j$(nproc)
ctest --test-dir build --output-on-failure
```

Or run the test binary directly:

```sh
./build/tests/hexcaster_tests
```

### Install LV2 bundle manually

```sh
cmake --install build
```

## Loading a NAM Model

Both the LV2 and CLAP plugins use a sidecar file to receive the model path, avoiding any in-plugin file browser requirement.

**Step 1 — write the model path to the sidecar file:**

```sh
mkdir -p ~/.config/hexcaster
echo "/path/to/your/model.nam" > ~/.config/hexcaster/model_path
```

**Step 2 — trigger the reload in your DAW:**

In Reaper (or any LV2/CLAP host), open the HexCaster FX window and set the **Model Reload** parameter from 0 to 1. The plugin loads the model on a background thread — audio continues uninterrupted and the model is live within about a second.

To load a different model, overwrite the sidecar file and toggle Model Reload again.

**Model path is saved with the project.** When you reopen a Reaper project, HexCaster restores the last loaded model automatically via the plugin state mechanism (LV2 state interface / CLAP state extension).

### Recommended models

HexCaster targets real-time performance on Raspberry Pi 5. For development on desktop hardware any NAM model works. For Pi deployment, prefer:

- WaveNet Nano / Feather / Lite variants
- LSTM models at 1×8 or 1×12 size

`.nam` files are available at [tonehunt.org](https://tonehunt.org).

## Signal Flow

HexCaster targets Cab Mode: the Pi drives a power amp into a physical guitar cabinet.

```
Input
  → Noise Gate
  → Detector HPF (envelope path only)
  → Envelope Follower
  → Input Gain (fixed)
  → Pre-Gain Modulation (Bloom)
  → Neural Amp Model (NAM)
  → Post-Gain Compensation (Bloom)
  → Post EQ (mid-sweep tone shaping)
  → Master Volume (fixed)
  → Output → Power Amp → Guitar Cabinet
```

The Bloom controller runs a single envelope follower (with a 100 Hz detector HPF) and drives both gain stages in opposite directions, keeping perceived volume stable:

```
reductionDb = BloomDepth * envelope
PreGain_dB  = BloomBasePre_dB  - reductionDb
PostGain_dB = BloomBasePost_dB + BloomCompensation * reductionDb
```

`BloomCompensation` is a ratio (default 0.5). At 1.0, output exactly compensates for input reduction.

The physical cabinet provides speaker filtering. No IR convolution stage.

## Development Status

| Phase | Status | Scope |
|-------|--------|-------|
| 1 | Done | Input Gain, NAM integration, ALSA standalone host, MIDI CC control |
| 2 | Done | Noise Gate, mid-sweep EQ, Master Volume |
| 3 | Done | Envelope follower, Bloom controller (pre/post gain modulation) |

## Performance Targets (Raspberry Pi 5)

- 48 kHz sample rate
- 64–128 sample buffer
- < 50% CPU total
- < 5 ms system latency
