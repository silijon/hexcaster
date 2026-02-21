# HexCaster

Neural dynamic gain amp platform for Linux / Raspberry Pi.

HexCaster is a DSP-core-first guitar amplifier engine built around Neural Amp Modeling (NAM). It provides dynamic pre/post gain control ("Bloom"), IR convolution cabinet simulation, parametric EQ, and algorithmic reverb. The core is real-time safe, framework-independent, and targets embedded deployment on Raspberry Pi 5.

Hosting wrappers (LV2, standalone daemon) are thin layers over the DSP core. The LV2 plugin is the primary development and validation target; the standalone runtime is the production deployment target.

## Repository Layout

```
hexcaster/
├── dsp/
│   ├── components/     # Individual DSP stages (GainStage, EQ, Reverb, ...)
│   └── pipeline/       # Signal flow composition (Pipeline, BloomController)
├── params/             # Parameter system (registry, smoothing, MIDI mapping)
├── hosts/
│   ├── lv2/            # LV2 plugin wrapper
│   └── standalone/     # Headless JACK/ALSA runtime
├── tests/              # Build validation and DSP unit tests
└── external/           # Git submodules (NeuralAudio, added in Phase 2)
```

## Dependencies

**Required:**
- CMake >= 3.18
- GCC with C++17 support

**For LV2 plugin:**
- `lv2-devel` (Fedora) / `liblv2-dev` (Debian/Ubuntu)

**For standalone runtime (Phase 2+):**
- JACK or PipeWire (JACK-compatible mode)
- ALSA

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
./build/hosts/standalone/hexcaster_standalone
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

**Cab Mode** (driving a physical guitar cabinet):

```
Input → Envelope Follower → Pre-Gain → NAM → Post-Gain → Post EQ → Output
```

**Direct Mode** (headphones / recording):

```
Input → Envelope Follower → Pre-Gain → NAM → IR Convolution → Reverb → Post EQ → Post-Gain → Output
```

The Bloom controller drives both pre-gain and post-gain from a single envelope follower, keeping them synchronised:

```
PreGain_dB  = BasePre  - A * envelope
PostGain_dB = BasePost + B * envelope
```

## Development Status

| Phase | Status | Scope |
|-------|--------|-------|
| 1 | In progress | Envelope, Bloom, EQ, IR convolution |
| 2 | Planned | NAM integration, Pi benchmarking |
| 3 | Planned | Reverb, dominance-linked control |
| 4 | Planned | Hardware, MIDI, preset system |

## Performance Targets (Raspberry Pi 5)

- 48 kHz sample rate
- 64–128 sample buffer
- < 50% CPU total
- < 5 ms system latency
