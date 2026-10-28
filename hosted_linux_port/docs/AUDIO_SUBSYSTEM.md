🌐 [한국어](kr/AUDIO_SUBSYSTEM.md) | **English**

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

---

## 6. Sample Rate Switching & Inter-Channel Phase Synchronization

### 6.1 Frequency Switching Audio Degradation
Repeated switching between tracks of different sample rates (e.g., 44.1 kHz $\leftrightarrow$ 48.0 kHz) led to progressive audio quality loss: vocals lost center presence, high frequencies suffered comb filtering, and the stereo image collapsed into a diffuse background reverberation ("muddier sound getting further away") despite unchanged RMS level.

### 6.2 Root Cause Analysis
1. **Inter-Channel Frame Alignment Desynchronization (`pcm_worker`):**
   When `write(eac_fd)` returned a partial byte count (non-multiple of 4), `written &= ~3` stripped unaligned bytes from the user-space counter while the kernel circular buffer advanced by the unaligned count. On the subsequent write iteration, the user-space buffer offset started at a 2-byte misaligned boundary, swapping Left and Right channels and introducing a 1-sample inter-channel time delay ($d = 1 / f_s \approx 22.7\,\mu\text{s}$). With successive rate transitions, this phase shift accumulated, producing severe comb filtering and destructive center-channel cancellation.
2. **PMIC FIR Reconstruction Filter Corruption:**
   Reconfiguring `AFE_PMIC_NEWIF_CFG0` (0x108), `AFE_I2S_CON1` (0x34), PMIC NewIF frequency (`0x4024`), and PMIC Codec rate (`0x4002`) while PMIC Downlink (`0x4014`) and PMIC Audio Digital Master (`0x4000`) were active disrupted the internal FIR decimation and interpolation filter delay lines in the MT6323, corrupting the reconstruction filter state.
3. **Stuck PMIC CDC Clock Sync Trigger:**
   The function `top_ctl_trigger()` calculated `toggle = ((~val) << 8) & 0x300` using register `0x4016`. Because bits 0 and 1 of `0x4016` read zero, shifting by 8 produced a constant `0x300`, preventing the necessary clock sync edge on subsequent transitions.
4. **Kernel Ring Buffer Stale Data & Unaligned Pointers:**
   Changing clock rates without resetting the 32 KB coherent DL1 DMA buffer left stale samples from the previous song playing at the new clock rate and allowed kernel read/write pointers to begin at arbitrary non-zero offsets. Concurrently, `sink_set_freq()` ran without holding `sink_lock()`, allowing `pcm_worker` to write audio data during hardware register programming.

### 6.3 Implementation
- **Zero-Padding Frame Alignment:** In `pcm_worker`, when `written & 3 != 0`, zero padding (`4 - unaligned` bytes) is written directly to `/dev/eac` to restore the kernel ring buffer to a 4-byte boundary. User-space pointers advance by the full frame size (`written + pad_len`), maintaining 4-byte stereo alignment and eliminating channel swapping or phase delay.
- **PMIC Gating:** In `sink_set_freq()`, PMIC downlink (`0x4014`) and master (`0x4000`) are gated to 0 before altering sample rate dividers, and re-enabled after registers and digital gain (`0x4006`, `0x4008` = 0 dB) are settled.
- **Ring Buffer Pointer Reset:** On sample rate changes, `AUDDRV_FREE_DL1_BUF`, `AUDDRV_ALLOCATE_DL1_BUF`, and `SET_MEMIF_FP` reset kernel circular buffer pointers to 0.
- **Toggling CDC Clock Trigger:** In `pcm-eac.c` and `unmute_eac.c`, `top_ctl_trigger()` toggles clock sync bits using `(val ^ 0x300) & 0x300`.
- **Concurrency Locking:** `sink_set_freq()` is wrapped in `sink_lock()` and `sink_unlock()`, clearing `pcm_data` under `pcm_mtx` so Rockbox feeds newly aligned frames only after hardware stabilization.

