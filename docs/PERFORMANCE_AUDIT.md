# HexCaster Realtime Performance and Latency Audit

Date: 2026-09-03

Status: Initial reliability/performance batch implemented on 2026-09-04.

Completed from the prioritized plan:

1. Opt-in fixed-storage realtime timing and xrun instrumentation (`--rt-metrics`)
2. Independent capture/playback ALSA negotiation and mismatch validation
3. Bounded handling for short ALSA reads and writes
4. Best-effort memory locking, stack prefaulting, and flush-to-zero setup
5. DSP/NAM preparation and prewarming after ALSA negotiation
8. Caching of unchanged parameter application and noise-gate/Bloom derived values
9. TUI-only level metering and default-Bloom chord-score tracking

The remaining significant reliability risk is independent input/output clock
drift. It requires measured validation and, if necessary, a later asynchronous
capture/playback design; it is intentionally not hidden by this initial batch.

## Objective

Reduce live-play latency as far as practical on Raspberry Pi 5 while preserving
realtime reliability and achieving zero xruns. Average CPU utilization is useful,
but the controlling metric is worst-case completion time relative to each ALSA
period deadline.

The likely initial production target is:

- 48 kHz
- NAM A2 Lite
- 64-frame ALSA periods
- Two periods per ALSA buffer
- A 1.333 ms processing deadline
- Approximately 2.667 ms of period-level capture/playback buffering before
  hardware, USB scheduling, and codec delay

A2 Full should remain selectable. A 48-frame period may prove practical after
the reliability and hot-path work below. A 32-frame period provides only 667 us
per deadline and should be treated as an aggressive target until the system is
instrumented and the ALSA issues are fixed.

## Current Realtime Path

The standalone daemon currently follows this path:

```text
Blocking ALSA capture
  |
  v
PCM-to-float conversion
  |
  v
Input level meter
  |
  v
Bloom detector/controller
  |
  v
Noise Gate
  |
  v
Input Gain
  |
  v
Bloom Pre Gain
  |
  v
NAM input calibration gain
  |
  v
NeuralAudio native A2 inference
  |
  v
NAM output normalization gain
  |
  v
Bloom Post Gain
  |
  v
Low + High Shelf EQ
  |
  v
Master Gain
  |
  v
Output level meter
  |
  v
Float-to-PCM conversion
  |
  v
Blocking ALSA playback
```

Relevant implementation:

- [`hosts/standalone/main.cpp`](hosts/standalone/main.cpp)
- [`hosts/standalone/alsa_audio_engine.cpp`](hosts/standalone/alsa_audio_engine.cpp)
- [`dsp/pipeline/src/pipeline.cpp`](dsp/pipeline/src/pipeline.cpp)
- [`dsp/components/src/nam_stage.cpp`](dsp/components/src/nam_stage.cpp)

## Highest-Priority Reliability Findings

### 1. Capture and playback negotiation can disagree

The ALSA engine has a single `actualRate_` and `actualFrames_` for two
independently opened devices. Capture negotiation writes these fields, then
playback negotiation overwrites them.

Relevant files:

- [`hosts/standalone/alsa_audio_engine.h`](hosts/standalone/alsa_audio_engine.h)
- [`hosts/standalone/alsa_audio_engine.cpp`](hosts/standalone/alsa_audio_engine.cpp)

The iRig and HiFiBerry can independently negotiate different period sizes. The
processing buffers and capture reads ultimately use the playback result. A
mismatch could produce invalid reads or a buffer overrun.

Recommended changes:

- Store capture and playback rate, period size, period count, and total buffer
  size independently.
- Query final values after ALSA commits each device's hardware parameters.
- Under the current synchronous architecture, require matching rates and period
  sizes or fail startup with a clear diagnostic.
- Log all requested and negotiated values.
- Load or prepare the final NAM and DSP graph after the audio configuration is
  known.

This work is a prerequisite for safely testing periods below 128 frames.

### 2. Short ALSA reads and writes are not handled completely

A short capture read currently skips processing without feeding playback. A
short playback write is not completed. A temporary scheduling event can
therefore become an underrun.

Implement bounded read/write completion handling and maintain separate counters
for:

- Capture overruns
- Playback underruns
- Short reads
- Short writes
- ALSA recoveries
- Failed recoveries

The audio thread must update fixed counters only. Logging and formatting should
remain on a control or status thread.

### 3. The input and output devices do not share a clock

The iRig USB capture clock and HiFiBerry playback clock are independent. Both
may nominally run at 48 kHz while their physical rates differ slightly. The
current single-threaded synchronous loop has no drift compensation.

This is a major long-duration zero-xrun risk, and smaller buffers make it surface
sooner.

Recommended progression:

1. Instrument and quantify drift during sustained testing.
2. Retain the simpler synchronous architecture if it remains stable for the
   required duration.
3. If drift causes xruns, separate capture and playback timing domains with a
   bounded ring buffer and adaptive asynchronous sample-rate correction.
4. Keep the ring buffer as shallow as measured clock and scheduler behavior
   permit.

A stable 64-frame system with a controlled safety period is preferable to an
unstable 32-frame system.

### 4. Memory locking is permitted but not enabled by the daemon

The systemd service permits unlimited locked memory and realtime scheduling in
[`systemd/hexcaster.service.in`](systemd/hexcaster.service.in). The audio thread
requests `SCHED_FIFO` priority 70, but the daemon does not call:

```cpp
mlockall(MCL_CURRENT | MCL_FUTURE);
```

It also does not prefault the audio thread stack. Add both before realtime
processing starts. NAM and pipeline prewarming are already useful, but they do
not prevent unrelated page faults.

### 5. Explicit denormal protection is missing

Filters and envelope followers can enter subnormal floating-point ranges during
long decays. Subnormal processing can create unpredictable timing spikes.

Set flush-to-zero behavior on the audio thread, including AArch64 FPCR `FZ`
behind appropriate platform guards. Persistent filter and envelope states may
also be snapped to zero below a suitably tiny threshold.

This is primarily a worst-case latency improvement rather than an average CPU
improvement.

## Period and Latency Budget

At 48 kHz:

| Period | Processing deadline | Approximate two-period I/O contribution |
| ---: | ---: | ---: |
| 128 frames | 2.667 ms | 5.333 ms |
| 96 frames | 2.000 ms | 4.000 ms |
| 64 frames | 1.333 ms | 2.667 ms |
| 48 frames | 1.000 ms | 2.000 ms |
| 32 frames | 0.667 ms | 1.333 ms |

The final column is only a period-level estimate. USB scheduling, ALSA drivers,
playback hardware, converters, and codec filters add latency. Final round-trip
latency should be measured with an electrical loopback and an impulse or similar
known stimulus.

The daemon accepts `--buffer`, and the systemd configuration exposes
`HEXCASTER_BUFFER_FRAMES`. ALSA is configured using
`snd_pcm_hw_params_set_period_size_near()`, so requesting 64 frames does not
guarantee that ALSA selected 64 frames. The final value must be queried and
reported.

## DSP Hot-Path Opportunities

### Cache parameter-derived values

Several expensive calculations currently happen every block even when their
inputs have not changed:

- Input and master gain setters calculate dB-to-linear conversion in
  [`dsp/components/src/gain_stage.cpp`](dsp/components/src/gain_stage.cpp).
- Bloom sensitivity performs another dB conversion in
  [`dsp/pipeline/src/bloom_controller.cpp`](dsp/pipeline/src/bloom_controller.cpp).
- Noise-gate coefficients are recalculated every block, including `pow()` and
  multiple `exp()` calls, in
  [`dsp/components/src/noise_gate.cpp`](dsp/components/src/noise_gate.cpp).
- The standalone host loads the parameter atomics and writes them into component
  atomics every block in
  [`hosts/standalone/main.cpp`](hosts/standalone/main.cpp).

Recommended changes:

- Cache the last applied control values.
- Call component setters only when a value changes.
- Convert dB, time constants, and filter controls to linear values or
  coefficients only when their controlling value changes.
- Preserve the current per-sample smoothing of gain transitions.
- Keep control-to-DSP communication lock-free.

The shelf-filter coefficient caching is a useful existing pattern.

### Reduce full-buffer gain passes

The current graph includes separate passes for:

- Input gain
- Bloom pre-gain
- NAM input calibration
- NAM output normalization
- Bloom post-gain
- Master gain

The following pre-NAM values can potentially be combined into one smoothed gain
stage:

```text
Input Gain + Bloom Pre + NAM Calibration
```

The following post-NAM values can potentially be combined:

```text
NAM Output Gain + Bloom Post
```

This can remove memory passes and multiplications. It must preserve the intended
smoothing semantics and should therefore follow profiling and lower-risk
optimizations.

A conservative first step would be to give `NamStage` an already combined
pre-NAM gain target while retaining equivalent smoothing behavior.

### Avoid unnecessary NAM copies

`NamStage` writes inference into a private output vector and then either applies
output gain or copies the result into the pipeline buffer:

- [`dsp/components/src/nam_stage.cpp`](dsp/components/src/nam_stage.cpp)

A ping-pong processing graph could let NAM write directly into an alternate
pipeline buffer. This may remove a copy or gain pass, but it is a broader graph
change and should only be done if profiling shows material memory-bandwidth cost.

### Avoid unused production diagnostics

The level meter performs full-buffer scans and logarithms even when the daemon is
headless. Chord-score tracking also performs per-sample slope and peak analysis,
although its primary consumer is diagnostic or experimental behavior.

Recommended options:

- Do not run meters unless the TUI or another status consumer requests them.
- Calculate dB outside the audio thread from stored linear peak values.
- Add a production option to omit chord-score diagnostics while retaining the
  required Bloom envelope processing.
- Alternatively, update diagnostic values at a reduced rate.

### Bypass unity shelf filters safely

Both shelf biquads still execute at exactly 0 dB. Add an identity bypass when a
shelf is at unity. Filter state must be managed when entering and leaving bypass
so stale state cannot create a transient.

### Virtual dispatch is currently low priority

The graph performs virtual calls for each stage and invokes controller hooks
between stages. This cost should be small relative to NAM, particularly at
64-128 frames. Do not flatten or specialize the graph unless profiling shows it
is material.

## NeuralAudio and NAM A2 Findings

HexCaster pins NeuralAudio commit:

```text
e59cd5d473d5b5772c69e755d7c5bc1007cff9ab
```

The exact pinned source and its submodules were inspected for this audit.

The repository is already using the preferred general dependency architecture:

- NeuralAudio native static A2 enabled
- NAMCore fallback enabled
- RTNeural disabled
- `float` samples throughout
- NeuralAudio FastMath enabled
- NAM A2 fast path enabled
- A1 static implementations disabled

Relevant configuration:

- [`cmake/dependencies.cmake`](cmake/dependencies.cmake)
- [`CMakeLists.txt`](CMakeLists.txt)

Standard A2 Lite and A2 Full models use NeuralAudio native static templates.
NAMCore is the fallback for nonstandard A2 variants. `NAM_USE_INLINE_GEMM`
therefore does not affect the normal static Lite/Full path and is relevant only
when benchmarking fallback models.

### `WAVENET_FRAMES`

HexCaster currently forces:

```cmake
WAVENET_FRAMES=128
```

This is a compile-time maximum, not a requirement to process 128 samples per
call. The model can process 64, 48, or 32 frames while compiled with a maximum of
128.

Reducing the compiled maximum to 64 may reduce static working storage and improve
cache behavior. Benchmark:

- `WAVENET_FRAMES=128`
- `WAVENET_FRAMES=64`
- Optionally `WAVENET_FRAMES=32` if no supported runtime configuration needs a
  larger period

The host also contains a hardcoded 128-frame inference chunk limit in
[`dsp/components/src/nam_stage.cpp`](dsp/components/src/nam_stage.cpp). Replace
this duplication with a shared or generated build definition so the host and
NeuralAudio cannot disagree.

### ARM multi-frame convolution

The pinned NeuralAudio version conditionally enables four-frame 8x8 convolution
processing on AArch64 for sufficiently new compilers:

- GCC 15 or newer
- Clang 21 or newer

Older compilers leave this optimization disabled. Do not force it blindly;
benchmark both performance and output correctness. Candidate periods of 128, 96,
64, 48, and 32 are all multiples of four and are suitable for testing this path.

## HexCaster Build Configuration

The Release build already enables:

- `-O3`
- `-ffast-math`
- `-mcpu=cortex-a76` on Raspberry Pi 5
- LTO/IPO when supported

See [`CMakeLists.txt`](CMakeLists.txt).

These are sound defaults. Further opportunities are:

- Disable global position-independent code for standalone-only Pi builds. PIC is
  needed for plugin targets but not for the daemon. The likely benefit is small.
- Build only standalone and tests for the Pi deployment profile. This primarily
  saves build time and dependency work, not audio-thread CPU.
- Consider profile-guided optimization after structural hot-path work is stable.
- Verify final flags with verbose compiler and linker output.
- Keep the entire audio path in `float`; introducing `double` is unnecessary.

## Realtime Instrumentation

Add optional instrumentation around:

1. ALSA capture and input PCM conversion
2. Parameter synchronization
3. Bloom processing
4. Noise gate
5. NAM inference
6. Post-NAM DSP
7. Output PCM conversion
8. ALSA playback

Use a monotonic raw clock with fixed-size counters or histograms. The audio
thread must not allocate, format strings, or log.

Collect:

- Mean processing time
- p95
- p99
- p99.9
- Maximum processing time
- Deadline misses
- Capture overruns
- Playback underruns
- Short I/O operations
- ALSA recovery events
- Major and minor page faults
- CPU frequency
- CPU temperature
- Throttling state
- End-to-end measured latency

For production qualification, maximum observed processing time should remain
comfortably below the period. An initial acceptance target is a maximum no more
than 60-70% of the period under thermal and system stress, with zero xruns. p99
alone is not sufficient for realtime qualification.

## Benchmark Matrix

| Variable | Values |
| --- | --- |
| Model | A2 Lite, A2 Full |
| Period | 128, 96, 64, 48, 32 frames |
| A2 compile-time frame maximum | 128, 64; optionally 32 |
| Compiler | Current GCC, newer GCC or Clang when practical |
| Multi-frame convolution | Disabled, supported native setting |
| Runtime | Headless systemd service |
| Screening duration | At least 15 minutes |
| Qualification duration | At least 1 hour |
| Final soak duration | At least 8 hours |
| System load | Idle and controlled CPU/I/O stress |
| Thermal state | Cold start and fully heat-soaked |

Reject a configuration if it:

- Produces any xrun
- Repeatedly throttles
- Accumulates clock-drift failures
- Negotiates unexpected ALSA settings
- Has inadequate worst-case deadline headroom

## Raspberry Pi System Tuning

After application correctness and instrumentation work:

- Use the `performance` CPU-frequency governor.
- Ensure active cooling prevents frequency throttling.
- Assign the audio thread to a measured quiet CPU core.
- Keep MIDI, TUI, logging, and control work off that core.
- Inspect USB, I2S, and timer IRQ affinity before choosing the audio core.
- Consider PREEMPT_RT if stock-kernel scheduling tails prevent reliable 48- or
  32-frame operation.
- Continue using direct `hw:` ALSA devices to avoid hidden plugin resampling and
  buffering.
- Do not run the TUI during final latency qualification.
- Disable unnecessary services or radios only when measurement shows that they
  interfere with deadlines.

CPU and IRQ affinity should be derived from measurements on the target Pi rather
than copied from generic tuning guidance.

## Prioritized Implementation Plan

1. Add realtime timing, deadline, short-I/O, and xrun instrumentation.
2. Store and validate independent capture/playback ALSA negotiation results.
3. Correct short read/write handling.
4. Add `mlockall`, stack prefaulting, and explicit denormal handling.
5. Prepare and prewarm the final graph after actual ALSA rate and period are
   known.
6. Establish A2 Lite and Full baselines at 128 frames.
7. Test 96 and 64 frames; select 64 if sustained deadline margin is healthy.
8. Cache parameter-derived values and remove unchanged per-block setter work.
9. Disable unnecessary headless metering and optional chord-score work.
10. Add safe unity shelf bypasses.
11. Benchmark `WAVENET_FRAMES=64`, newer compilers, and supported multi-frame
    convolution.
12. Test 48-frame operation.
13. Test 32 frames only if 48-frame maximum processing time remains well below
    approximately 400-450 us under stress.
14. Measure electrical loopback latency and perform an eight-hour or longer
    clock-drift soak.
15. If drift produces xruns, implement bounded asynchronous capture/playback
    clock adaptation.

The recommended first implementation batch is items 1-5. They improve realtime
reliability and make every later optimization measurable while preserving the
existing DSP sound and signal ordering.
