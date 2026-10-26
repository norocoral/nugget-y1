# Audio Subsystem: MediaTek EAC & MT6323

This document details the audio hardware architecture, driver interface, sample rate switching, transient pop mitigation, and gain staging on the Innioasis Y1 hosted Rockbox port.

---

## 1. Hardware Signal Path

```mermaid
flowchart LR
    RB["Rockbox Codec Engine\n(16-bit Stereo PCM)"]
    DEV["/dev/eac\n(MediaTek EAC Device Node)"]
    DSP["MT6572 Audio DSP\n(I2S Digital Filters)"]
    PMIC["MT6323 PMIC\n(Stereo DAC & Headphone Amp)"]
    HP["3.5mm Headphone Jack\n(accdet Sensing)"]

    RB -->|write PCM frames| DEV
    DEV -->|DMA FIFO| DSP
    DSP -->|I2S Bus| PMIC
    PMIC -->|Analog Out| HP
```

---

## 2. Kernel Interface: `/dev/eac`

Downstream MediaTek kernel 3.4.67 routes digital PCM audio through the Enhanced Audio Controller character device (`/dev/eac`) instead of standard ALSA PCM devices.

### 2.1 Configuration Parameters
- Sample Rates: 44,100 Hz and 48,000 Hz native hardware clocking.
- Encoding: 16-bit signed integer (`int16_t`), little-endian, stereo interleaved (L, R, L, R...).
- Frame Size: 4 bytes per stereo sample pair.
- Buffer Depth: 8,192 bytes (2,048 stereo frames, ~46.4ms buffer latency at 44.1 kHz).
- Thread Scheduling: `SCHED_FIFO` real-time priority 80.

---

## 3. Transient Pop & Click Elimination

### 3.1 Failure Mechanisms
1. **PLL Divisor Shifts:** Switching audio clocks between 44.1 kHz and 48.0 kHz abruptly disrupted the MT6323 delta-sigma modulator, causing DC offset steps.
2. **Buffer Flush Truncation:** Halting audio without draining the FIFO left the output biased away from 0V, generating a transient click when the amplifier powered down.
3. **FM Radio Sharing:** Shared analog paths caused line interference during digital audio initialization.

### 3.2 Implemented Fixes
- **Digital Settling Buffer:** Before closing the audio device or altering sample rate dividers, 256 samples of zero-amplitude PCM ($0x0000$) are written to `/dev/eac` to allow the analog reconstruction filters to settle to neutral ground.
- **Hardware Mute Sequencing:** The analog headphone amplifier line is muted prior to PLL clock changes, and unmuted only after valid frames pre-fill the DMA ring buffer.
- **Identical Rate Bypass:** Track transitions with identical sample rates bypass driver reconfiguration entirely for seamless gapless playback.

---

## 4. Gain Staging & Volume Control

Gain management combines digital attenuation with analog amplification:
- **Digital Range (-74 dB to 0 dB):** Handled by Rockbox's software volume dither engine. Digital amplification never exceeds 0 dBFS to prevent clipping.
- **Analog Headroom (0 dB to +12 dB):** Handled by the MT6323 integrated analog headphone amplifier stages, providing driving power for higher impedance headphones without digital distortion.

---

## 5. Headphone Detection & Disconnect

The 3.5mm jack uses an analog impedance sensing switch wired to the `accdet` driver.
- Disconnect: Monitored via `/sys/class/switch/h2w/state`. When unplugged, Rockbox immediately invokes `pause_action()`.
- Reconnect: Playback resumes automatically when enabled in `Settings -> Playback Settings -> Pause on Headphone Unplug`.

