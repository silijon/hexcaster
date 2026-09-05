# Raspberry Pi 5 I2S DMA failure at low ALSA buffer sizes

Status: investigation paused while performance work is deferred  
Last updated: 2026-09-05

## Summary

HexCaster can currently operate with the iRig HD 2 as its USB capture device
and the HiFiBerry Amp100 as its I2S playback device at 48 kHz with a 256-frame
ALSA period. A short test on the master branch allowed the service to be
started and stopped repeatedly at that setting.

At a 128-frame period, the same master-branch build starts successfully but,
after approximately 10 to 30 seconds, reports a playback xrun (`-EPIPE`,
"Broken pipe"). HexCaster then enters its existing ALSA recovery path. Repeated
playback recovery is followed by a kernel fault in the Raspberry Pi 5 RP1
DesignWare DMA path used by the HiFiBerry I2S output.

Once that kernel fault occurs, the playback device may remain unusable or
report `Device or resource busy` even though HexCaster has exited and `fuser`
finds no process holding `/dev/snd/*`. The machine must be rebooted to restore a
clean DMA/ALSA state.

This behavior is reproducible without the performance changes from the
`perf-optimizations` branch. Those changes are therefore not the root cause of
the low-buffer kernel failure, and performance work is intentionally out of
scope until this issue is understood.

## Tested system

- Host: Raspberry Pi 5 Model B Rev 1.1
- OS: Debian Trixie / Raspberry Pi packages
- Kernel: `6.18.39+rpt-rpi-2712`, package `1:6.18.39-1+rpt1`
- Architecture: AArch64
- Capture: iRig HD 2 over USB ALSA
- Playback: HiFiBerry Amp100 over I2S ALSA
- Sample rate: 48 kHz
- ALSA periods: 2
- Capture format observed: `S24_3LE`, mono
- Playback format observed: `S16_LE`, stereo
- DSP daemon: native C++ HexCaster standalone host

The Pi 5 boots the expected `/boot/firmware/kernel_2712.img`. No alternative
kernel was installed, and the configured APT repository offered the same
`6.18.39` package as both the installed and candidate version.

## Reproduction results

### 256-frame period

Using a clean branch from master, without the pending performance work:

- HexCaster started successfully.
- Audio operated normally during the short sanity test.
- The service could be stopped and started repeatedly.
- No DMA kernel fault was observed during that test.

This establishes 256 frames as the current provisional safe setting. It does
not yet establish indefinite stability: the 256-frame configuration still
needs a sustained soak test because a larger buffer could merely increase the
time required for independent capture/playback clocks to drift to an xrun.

### 128-frame period

Using the same master-branch source:

1. HexCaster started and initially processed audio.
2. After roughly 10 to 30 seconds, the daemon repeatedly logged:

   ```text
   Playback error: Broken pipe -- recovering
   ```

3. The kernel began logging:

   ```text
   dma dma2chan2: BUG: dma2chan2, IRQ with no descriptors
   ```

4. The kernel then faulted on the poison address `deaddeaddeaddead` with this
   significant call path:

   ```text
   dma_pool_alloc
   axi_desc_get
   dw_axi_dma_set_hw_desc
   dw_axi_dma_chan_prep_cyclic
   snd_dmaengine_pcm_trigger
   dmaengine_pcm_trigger
   soc_pcm_trigger
   snd_pcm_do_start
   snd_pcm_start
   __snd_pcm_lib_xfer
   ```

5. The kernel reported that the fault happened in the context of the
   `hexcaster` process. This identifies the userspace syscall that entered the
   faulty kernel path; it does not by itself indicate userspace memory
   corruption in HexCaster.
6. After the fault, the service could no longer be used reliably and the PCM
   device could appear busy with no userspace owner.
7. A reboot cleared the bad state. The subsequent boot contained no
   `dma2chan2`, kernel-oops, or `deaddead` messages.

## Matching upstream kernel reports

The observed failure is an exact match for the open Raspberry Pi Linux issue:

- [Pi 5 kernel crash in dmaengine ALSA path (dw_axi_dma) during I2S playback
  (PCM512x), raspberrypi/linux#7320](https://github.com/raspberrypi/linux/issues/7320)

That report covers Raspberry Pi 5 systems using PCM512x DAC HATs and includes
the same `IRQ with no descriptors`, `deaddeaddeaddead`, `dma_pool_alloc`,
`dw_axi_dma_chan_prep_cyclic`, and ALSA trigger call chain.

A related open report describes failure to restart the Pi 5 DesignWare I2S DMA
path after ALSA issues a STOP:

- [designware_i2s BCM2712: trigger(STOP) permanently breaks DMA restart,
  raspberrypi/linux#7322](https://github.com/raspberrypi/linux/issues/7322)

Issue #7322 was reported against kernel 6.12.75. Therefore, moving from 6.18
to an arbitrary 6.12 kernel is not a justified workaround. A kernel change
should only be made after identifying and testing a version known not to
contain the fault, with matching kernel image, modules, device trees, and
initramfs.

## Relevant current HexCaster behavior

The master-branch ALSA implementation is in
`hosts/standalone/alsa_audio_engine.cpp`.

The important behavior is:

- `AlsaAudioEngine::openHandle()` opens blocking ALSA capture and playback
  handles and configures a start threshold and availability minimum of one
  period.
- `AlsaAudioEngine::run()` prepares both devices and primes playback before
  entering a synchronous `capture → DSP → playback` loop.
- A capture or playback error calls `AlsaAudioEngine::recoverBoth()`.
- `recoverBoth()` drops both PCM streams, prepares both streams, and primes
  playback again.
- Priming a prepared playback stream causes ALSA to start playback after its
  configured threshold is reached. The failing kernel trace shows the fault
  during this DMA start path.
- `AlsaAudioEngine::stop()` calls `snd_pcm_drop()` on capture from a different
  thread to interrupt a blocking read.
- `AlsaAudioEngine::close()` drops and closes both handles.

The installed service template is
`hosts/standalone/deployment/hexcaster.service.in`. It currently specifies
`Restart=on-failure`. Automatic process restart is unsafe during this
investigation because repeated I2S STOP/START cycles can exercise the affected
kernel path after an xrun.

## Failure chain

The evidence currently supports this sequence:

```text
128-frame operation
        |
        v
initial playback xrun (-EPIPE)
        |
        v
HexCaster recoverBoth()
  drop capture + playback
  prepare capture + playback
  prime/restart playback
        |
        v
RP1 DesignWare DMA restart path
        |
        v
IRQ with no descriptors / non-idle DMA
        |
        v
kernel oops in dma_pool_alloc
        |
        v
corrupted ALSA/DMA state until reboot
```

Two problems must therefore be treated separately:

1. **Why the first playback xrun occurs at 128 frames.**
2. **Why recovery from that xrun crashes the kernel.**

The kernel trace and upstream report answer the second question. They do not
yet answer the first.

## Current hypotheses for the initial xrun

### Independent hardware clocks

The iRig USB capture device and HiFiBerry I2S playback device do not share a
hardware clock. Both nominally run at 48 kHz, but their actual rates can differ
by tens or hundreds of parts per million. The current synchronous loop always
reads and writes the same number of frames. With no clock-domain buffer or
adaptive sample-rate conversion, one finite ALSA buffer must eventually empty
or fill.

The observed playback underrun means playback consumed samples faster than the
capture/DSP path supplied them during that run. A 10-to-30-second time to xrun
is plausible for clock drift with only two 128-frame periods, but this remains
a hypothesis until measured.

### Realtime deadline or scheduling miss

A DSP block, scheduler delay, interrupt burst, page fault, thermal event, or
other latency spike could exceed the 128-frame period budget:

```text
128 / 48000 = 2.667 ms
```

At 256 frames the period budget is:

```text
256 / 48000 = 5.333 ms
```

The successful short 256-frame test is consistent with increased scheduling
headroom, but does not distinguish deadline misses from clock drift.

### Combination

Small clock drift may move playback close to the edge of its buffer, after
which an otherwise tolerable scheduling spike causes the first xrun.

## Observations that are not the primary failure

After a clean reboot, two unrelated boot messages remained:

```text
usbhid 3-1:1.3: couldn't find an input interrupt endpoint
raspberrypi-firmware ... Request 0x00030097 returned status 0x80000001
```

The HID message concerns one interface of a composite USB device and is not an
I2S DMA fault. The firmware request is `GET_EEPROM_UPDATE_STATUS`; it has been
described by a Raspberry Pi engineer as not affecting anything significant.
Neither message contains the DMA failure signature documented here.

USB audio descriptor warnings about unusually large mixer volume ranges are
also separate from the RP1 DMA crash. HexCaster should not infer calibration
from those mixer ranges or manipulate them speculatively.

## Safe operating guidance

Until this is resolved:

- Use a 256-frame period as the provisional configuration.
- Do not assume 256 frames is indefinitely safe until it passes a long soak.
- Do not run 128 frames with the current automatic recovery behavior.
- Avoid repeated start/stop/restart cycles after any I2S error.
- Disable automatic systemd restart while intentionally reproducing failures.
- If any `dma2chan2`, `IRQ with no descriptors`, `non-idle`, kernel-oops, or
  `deaddead` message appears, stop the test and reboot.
- Do not judge subsequent ALSA behavior after a kernel oops; the kernel is
  already in an invalid state.
- Do not attempt to repair a post-oops busy device by repeatedly reopening it.

Useful checks after a suspected failure:

```sh
sudo fuser -v /dev/snd/*
grep -R . /proc/asound/card*/pcm*/sub*/status
sudo dmesg | grep -E 'dma2chan2|IRQ with no descriptors|non-idle|deaddead|Internal error|Oops'
```

An empty `fuser` result combined with the kernel-oops signature indicates
kernel/driver state corruption rather than a leaked HexCaster file descriptor.

## Recommended investigation sequence

No performance optimization work should be resumed until the system has a
safe failure mode and the initial xrun is characterized.

1. **Add a diagnostic fail-fast xrun policy.** On the first capture or playback
   xrun, record fixed-size realtime-safe diagnostics and leave the audio loop
   without calling `recoverBoth()`. Do not automatically restart the service.
   This is intended to protect the kernel while gathering evidence, not to be
   the final live-audio policy.
2. **Collect processing metrics at 256 frames.** Measure DSP callback mean,
   p95, p99, maximum, deadline misses, CPU use, thermals, and xruns over a
   sustained run.
3. **Soak 256 frames.** Run long enough to determine whether 256 frames prevents
   the first xrun or merely delays it.
4. **Test 128 frames only with fail-fast enabled.** Capture elapsed time to the
   first xrun, which side failed, processing-time distribution, and ALSA status
   immediately before failure. Reboot if the kernel emits any DMA warning.
5. **Measure clock drift.** Track capture and playback hardware/application
   pointers or buffer occupancy over time without logging from the realtime
   callback. Determine whether playback fill moves steadily toward underrun.
6. **Design clock-domain handling.** If drift is confirmed, decouple capture
   and playback with fixed-capacity realtime-safe buffering and adaptive
   sample-rate conversion. The playback stream should remain continuously open
   and should not require routine DMA STOP/START recovery.
7. **Track the kernel issue.** Test a new kernel only when there is evidence of
   a relevant fix. Retain a known-bootable kernel and matching modules as a
   rollback option.

## Success criteria

The issue is not considered resolved until all of the following hold:

- Sustained 128-frame operation produces no xruns.
- Capture/playback clock differences are handled without buffer exhaustion.
- No routine path drops and restarts the HiFiBerry stream.
- Start and stop are reliable on a healthy kernel.
- An audio failure cannot cause a rapid recovery or systemd restart storm.
- Kernel logs remain free of RP1/DesignWare DMA warnings and faults.
- The system survives a long thermal and realtime soak test.

## Unresolved questions

- Is the first 128-frame xrun caused primarily by clock drift, a realtime
  deadline miss, or both?
- How long can 256-frame operation run before an xrun under sustained use?
- What is the measured rate difference between the iRig and HiFiBerry clocks?
- Is there a Raspberry Pi kernel release or patch that fixes issues #7320 and
  #7322 for this exact HiFiBerry/PCM512x configuration?
- Can the relevant I2S modules be reloaded safely on this hardware, or is a
  reboot the only reliable recovery after STOP? Module reload is not assumed
  safe without a controlled test.
- What adaptive-resampling implementation offers the best realtime behavior
  and lowest added latency on the Pi 5?

