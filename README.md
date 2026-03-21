# HexCaster

Neural dynamic gain amp platform for Linux / Raspberry Pi.

HexCaster is a DSP-core-first guitar amplifier engine built around Neural Amp Modeling (NAM). It provides a noise gate, dynamic pre/post gain control ("Bloom"), and a mid-sweep EQ. The core is real-time safe, framework-independent, and targets embedded deployment on Raspberry Pi 5 driving a physical guitar cabinet.

Hosting wrappers (LV2, standalone daemon) are thin layers over the DSP core. The LV2 plugin is the primary development and validation target; the standalone runtime is the production deployment target.

## Repository Layout

```
hexcaster/
├── dsp/
│   ├── components/     # Individual DSP stages (GainStage, NamStage, NoiseGate, EQ, ...)
│   └── pipeline/       # Signal flow composition (Pipeline, BloomController)
├── params/             # Parameter system (registry, smoothing, MIDI mapping)
├── hosts/
│   ├── lv2/            # LV2 plugin wrapper
│   └── standalone/     # Headless JACK/ALSA runtime
├── tests/              # Build validation and DSP unit tests
└── external/           # Dependencies (NeuralAudio fetched via CMake FetchContent)
```

## Dependencies

**Required:**
- CMake >= 3.18
- GCC with C++20 support
- `libasound2-dev` (Debian/Ubuntu) / `alsa-lib-devel` (Fedora)

**For LV2 plugin (optional):**
- `liblv2-dev` (Debian/Ubuntu) / `lv2-devel` (Fedora)

## Building

### Configure

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Build options (all default ON):

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHEXCASTER_BUILD_LV2=ON \
  -DHEXCASTER_BUILD_STANDALONE=ON \
  -DHEXCASTER_BUILD_TESTS=ON
```

### Build LV2 plugin

```sh
cmake --build build --target hexcaster_lv2 -j$(nproc)
```

The plugin bundle is automatically installed to `~/.lv2/hexcaster.lv2/` after each build. No separate install step required during development.

To load in Reaper or another LV2 host, rescan plugins. The plugin appears as **HexCaster** under LV2.

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
  --midi-cc 7:MasterGain_dB \
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

The Bloom controller runs a single envelope follower and drives both gain stages in opposite directions, keeping them synchronised:

```
PreGain_dB  = BasePre  - A * envelope
PostGain_dB = BasePost + B * envelope
```

The physical cabinet provides speaker filtering. No IR convolution stage.

## Development Status

| Phase | Status | Scope |
|-------|--------|-------|
| 1 | Done | Input Gain, NAM integration, ALSA standalone host, MIDI CC control |
| 2 | In progress | Noise Gate, mid-sweep EQ, Master Volume |
| 3 | Planned | Envelope follower, Bloom (pre/post gain modulation), dominance-linked control |

## Performance Targets (Raspberry Pi 5)

- 48 kHz sample rate
- 64–128 sample buffer
- < 50% CPU total
- < 5 ms system latency
