# HexCaster

Neural dynamic gain amp platform for Linux / Raspberry Pi / macOS.

HexCaster is a DSP-core-first guitar amplifier engine built around NAM Architecture 2 (A2). It provides a noise gate, dynamic pre/post gain control ("Bloom"), and a mid-sweep EQ. The core is real-time safe, framework-independent, and targets embedded deployment on Raspberry Pi 5 driving a physical guitar cabinet.

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
│   └── standalone/     # Headless ALSA runtime (Linux)
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

## Configure, Build, Run, Debug, Install, and Clean

CMake configure commands are persistent per build directory. Re-run the
configure command whenever changing an option; CMake preserves options not
specified again. Use separate build directories for Release and Debug builds.

### CMake options

| Option | Default | Purpose |
|---|---:|---|
| `HEXCASTER_BUILD_STANDALONE` | `ON` | Linux ALSA daemon |
| `HEXCASTER_BUILD_TUI` | `ON` | Terminal UI in the standalone daemon |
| `HEXCASTER_BUILD_LV2` | `ON` | Linux LV2 development plugin |
| `HEXCASTER_BUILD_CLAP` | `ON` | Linux/macOS CLAP development plugin |
| `HEXCASTER_BUILD_TESTS` | `ON` | Unit and NAM integration tests |
| `HEXCASTER_EXPERIMENTAL_BLOOM` | `OFF` | Experimental chord-score Bloom envelope |
| `HEXCASTER_DEBUG_CHANNELS` | `OFF` | Extra internal DSP outputs in CLAP |
| `HEXCASTER_OPTIMIZE_RPI5` | `ON` | Cortex-A76 tuning on ARM64 |
| `HEXCASTER_ENABLE_LTO` | `ON` | Release-build link-time optimization |

`HEXCASTER_OPTIMIZE_RPI5` has no effect on x86-64. Disable it for a generic
ARM64 binary intended to run on processors other than the Raspberry Pi 5:

```sh
cmake -S . -B build-arm64 -DHEXCASTER_OPTIMIZE_RPI5=OFF
```

### Linux / Raspberry Pi dependencies

On Debian, Ubuntu, or Raspberry Pi OS:

```sh
sudo apt update
sudo apt install build-essential cmake git libasound2-dev
```

Optional LV2 development support requires:

```sh
sudo apt install liblv2-dev
```

CLAP, FTXUI, Eigen, JSON, NeuralAudio, and its pinned NAM dependencies are
fetched by CMake when their corresponding targets are enabled. The first
configure therefore requires network access.

### Raspberry Pi 5 production build

This is the recommended daemon-only Release configuration:

```sh
cmake -S . -B build-pi \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DHEXCASTER_BUILD_STANDALONE=ON \
  -DHEXCASTER_BUILD_TUI=ON \
  -DHEXCASTER_BUILD_LV2=OFF \
  -DHEXCASTER_BUILD_CLAP=OFF \
  -DHEXCASTER_BUILD_TESTS=ON \
  -DHEXCASTER_OPTIMIZE_RPI5=ON \
  -DHEXCASTER_ENABLE_LTO=ON

cmake --build build-pi --parallel "$(nproc)"
ctest --test-dir build-pi --output-on-failure
```

For the smallest production build after validation, tests and the TUI may be
disabled with `-DHEXCASTER_BUILD_TESTS=OFF -DHEXCASTER_BUILD_TUI=OFF`.

### Linux development build

Build every supported target and generate editor tooling:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHEXCASTER_BUILD_STANDALONE=ON \
  -DHEXCASTER_BUILD_TUI=ON \
  -DHEXCASTER_BUILD_LV2=ON \
  -DHEXCASTER_BUILD_CLAP=ON \
  -DHEXCASTER_BUILD_TESTS=ON

cmake --build build --parallel "$(nproc)"
ln -sf build/compile_commands.json compile_commands.json
```

If LV2 headers are unavailable, CMake warns and skips the LV2 target.

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

cmake --build build --parallel
```

The plugin bundle is automatically installed to `~/Library/Audio/Plug-Ins/CLAP/hexcaster.clap/` after each build. Rescan plugins in Reaper; the plugin appears as **HexCaster** under CLAP.

To generate a `compile_commands.json` for LSP/editor tooling, add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to the configure step, then symlink it to the project root:

```sh
ln -sf build/compile_commands.json compile_commands.json
```

The default Bloom implementation uses the attack/release envelope follower.
To build with the experimental chord-score power-curve envelope instead:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHEXCASTER_EXPERIMENTAL_BLOOM=ON
```

### Build LV2 plugin (Linux)

```sh
cmake --build build --target hexcaster_lv2 --parallel "$(nproc)"
```

The plugin bundle is automatically installed to `~/.lv2/hexcaster.lv2/` after each build. No separate install step required during development.

To load in Reaper or another LV2 host, rescan plugins. The plugin appears as **HexCaster** under LV2.

### Build CLAP plugin

```sh
cmake --build build --target hexcaster_clap --parallel "$(nproc)"
```

On macOS the bundle is installed to `~/Library/Audio/Plug-Ins/CLAP/hexcaster.clap/`. On Linux the plugin is installed to `~/.clap/hexcaster.clap`.

### Build standalone runtime

```sh
cmake --build build --target hexcaster_standalone --parallel "$(nproc)"
```

The executable is `build/hosts/standalone/hexcaster` even though the CMake
target is named `hexcaster_standalone`.

### Run the standalone daemon from the build tree

Run with separate input and output devices and the calibrated iRig input level:

```sh
./build/hosts/standalone/hexcaster \
  --model /path/to/model.nam \
  --input-device hw:CARD=V276,DEV=0 \
  --output-device hw:CARD=sndrpihifiberry,DEV=0 \
  --input-level-dbu 11.63 \
  --nam-quality auto \
  --buffer 128
```

Add `--tui` when the TUI was enabled. Device and complete CLI discovery:

```sh
./build/hosts/standalone/hexcaster --list-devices
./build/hosts/standalone/hexcaster --list-midi
./build/hosts/standalone/hexcaster --help
```

With MIDI control:

```sh
./build/hosts/standalone/hexcaster \
  --model /path/to/model.nam \
  --input-device hw:CARD=V276,DEV=0 \
  --output-device hw:CARD=sndrpihifiberry,DEV=0 \
  --input-level-dbu 11.63 \
  --midi-device hw:1,0,0 \
  --midi-cc 7:InputGain_dB \
  --midi-cc 1:BloomBasePre_dB \
  --tui
```

### Debug build

Use a separate directory and disable LTO so stack traces and debugger behavior
remain straightforward:

```sh
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHEXCASTER_ENABLE_LTO=OFF \
  -DHEXCASTER_BUILD_STANDALONE=ON \
  -DHEXCASTER_BUILD_TUI=ON \
  -DHEXCASTER_BUILD_LV2=OFF \
  -DHEXCASTER_BUILD_CLAP=OFF \
  -DHEXCASTER_BUILD_TESTS=ON

cmake --build build-debug --parallel "$(nproc)"
ctest --test-dir build-debug --output-on-failure
```

Run under GDB on Linux/Pi:

```sh
gdb --args ./build-debug/hosts/standalone/hexcaster \
  --model /path/to/model.nam \
  --input-device hw:CARD=V276,DEV=0 \
  --output-device hw:CARD=sndrpihifiberry,DEV=0 \
  --input-level-dbu 11.63 \
  --buffer 128
```

Inside GDB, use `run`, then `thread apply all bt` after a crash or interrupt.
Debugging can disturb realtime scheduling, so xrun behavior under GDB is not a
valid production-performance measurement.

To build CLAP with the internal DSP debug output channels:

```sh
cmake -S . -B build-debug-clap \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEXCASTER_DEBUG_CHANNELS=ON \
  -DHEXCASTER_BUILD_CLAP=ON \
  -DHEXCASTER_BUILD_LV2=OFF \
  -DHEXCASTER_BUILD_STANDALONE=OFF \
  -DHEXCASTER_BUILD_TESTS=OFF
cmake --build build-debug-clap --target hexcaster_clap --parallel
```

### Run tests

Build and run all registered tests:

```sh
cmake --build build --target \
  hexcaster_tests hexcaster_input_gain_tests hexcaster_nam_a2_tests \
  --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

List tests or run one group:

```sh
ctest --test-dir build --show-only
ctest --test-dir build -R input_gain_calibration --output-on-failure
ctest --test-dir build -R nam_a2 --output-on-failure
```

### Install as a Raspberry Pi system service

After configuring `build-pi` with `CMAKE_INSTALL_PREFIX=/usr` as shown above,
install and enable the service:

```sh
sudo cmake --build build-pi --target install-pi
```

`install-pi` installs `/usr/bin/hexcaster`, creates the dedicated `hexcaster`
system user, installs and enables `hexcaster.service`, and creates the model
directory. It deliberately does not start the service before it is configured.

Install the production model and configuration:

```sh
sudo install -o root -g hexcaster -m 0640 amp.nam \
  /var/lib/hexcaster/models/default.nam
sudo cp /etc/hexcaster/hexcaster.env.example \
  /etc/hexcaster/hexcaster.env
sudo editor /etc/hexcaster/hexcaster.env
sudo systemctl start hexcaster
```

The service is enabled for subsequent boots. Verify it with:

```sh
systemctl status hexcaster
journalctl -u hexcaster -f
```

To run the daemon manually on the Pi, stop the service first so it releases the
ALSA devices, then use the same arguments recorded in the unit/environment:

```sh
sudo systemctl stop hexcaster
/usr/bin/hexcaster --help
sudo systemctl start hexcaster
```

After rebuilding, update the installed daemon and restart it with:

```sh
cmake --build build-pi --parallel "$(nproc)"
ctest --test-dir build-pi --output-on-failure
sudo cmake --install build-pi --component hexcaster-runtime
sudo systemctl restart hexcaster
sudo systemctl status hexcaster --no-pager
```

`cmake --install ... --component hexcaster-runtime` updates installed files but
does not run `systemd-sysusers`, enable the unit, or restart the service. Use
`install-pi` for first installation and the component install for updates.

Use stable ALSA card names such as `hw:CARD=V276,DEV=0` in the environment
file; numeric `hw:2,0` identifiers can change across boots. The daemon runs as
an isolated service account with audio-group access, `SCHED_FIFO` permission,
automatic failure restart, locked-memory allowance, and a read-only system
filesystem.

### Normal CMake install and staging

Install all enabled installable targets using the configured prefix:

```sh
cmake --install build
```

To select a writable non-system prefix at install time:

```sh
cmake --install build --prefix "$PWD/install-root"
```

Preview installation into a staging root without modifying the host system:

```sh
DESTDIR="$PWD/stage" cmake --install build
```

LV2 and CLAP development targets also copy themselves to the current user's
plugin directory as a post-build step. This is separate from Pi daemon
installation.

### Clean and reconfigure

Remove compiled outputs while retaining the configured build tree and fetched
dependencies:

```sh
cmake --build build --target clean
cmake --build build-pi --target clean
cmake --build build-debug --target clean
```

To force a completely fresh configure, remove only the explicit build
directory and recreate it:

```sh
rm -rf -- build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

The CMake `clean` target does not remove LV2/CLAP copies previously placed in
user plugin directories. Remove only the HexCaster development copies with:

```sh
cmake -E remove_directory "$HOME/.lv2/hexcaster.lv2"
cmake -E remove -f "$HOME/.clap/hexcaster.clap"
```

On macOS:

```sh
cmake -E remove_directory \
  "$HOME/Library/Audio/Plug-Ins/CLAP/hexcaster.clap"
```

There is currently no CMake uninstall target for the Pi service. Installed
production files should be managed explicitly or by a future OS package.

Never run the recursive removal command with an empty variable, wildcard,
repository root, or home directory. Build directory names in this README are
deliberately explicit.

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

### A2 model selection

HexCaster accepts A2 models only. Direct A2 Lite and Full files are detected from their architecture and loaded through NeuralAudio's optimized native implementation. Packed A2 models containing both qualities are selected with:

- `--nam-quality auto` — select Lite from a packed model (the Pi-oriented default)
- `--nam-quality lite` — require/select Lite
- `--nam-quality full` — require/select Full

Non-standard A2 models fall back to NAMCore. RTNeural is not built or used.

`.nam` files are available at [tonehunt.org](https://tonehunt.org).

## Signal Flow

HexCaster targets Cab Mode: the Pi drives a power amp into a physical guitar cabinet.

```
Input
  → Noise Gate
  → Detector HPF (envelope path only)
  → Envelope Follower
  → Model Calibration + User Input Trim
  → Pre-Gain Modulation (Bloom)
  → Neural Amp Model (NAM)
  → Post-Gain Compensation (Bloom)
  → Post EQ (high/low shelf tone shaping)
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
