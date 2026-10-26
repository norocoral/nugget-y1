# Technical Findings: Innioasis Y1 (MT6572) Hosted Linux Port

This document records the technical architecture, reverse-engineered hardware interfaces, subsystem implementations, and bug fixes for the Innioasis Y1 hosted Rockbox port on downstream Linux kernel 3.4.67.

---

## 1. Hardware Specifications & Driver Interfaces

| Subsystem | Hardware Component | Kernel Interface / Node |
| :--- | :--- | :--- |
| SoC | MediaTek MT6572 (2x Cortex-A7 @ 1.2 GHz, 28nm) | Downstream Linux 3.4.67 |
| PMIC / Audio Codec | MediaTek MT6323 | `/dev/eac`, `/sys/devices/platform/mt-pmic` |
| RAM | 512 MB LPDDR2 | Standard Linux virtual memory |
| Internal Storage | 4 GB eMMC (`mmcblk0`) | Partition table ignored by Rockbox |
| External Storage | MicroSD slot (`mmcblk1p1`) | `/dev/block/mmcblk1p1` mounted at `/mnt/sdcard` |
| Display | 2.8" TFT LCD (480x360 native, 32bpp BGRA) | `/dev/fb0` |
| Input | Capacitive click wheel + 5 tactile keys + power key | `/dev/input/event0` - `/dev/input/event3` |
| Headphone Jack | 3.5mm stereo with MFB hook and insert switch | `/sys/class/switch/h2w`, `/dev/input/event*` |
| USB | Micro-USB OTG / Gadget | ConfigFS gadget (`g_mass_storage`) |

---

## 2. Audio Subsystem (`/dev/eac`)

### 2.1 Interface & Data Flow
The MT6572 ALSA subsystem is largely bypassed in downstream MediaTek kernels in favor of the Enhanced Audio Controller character device (`/dev/eac`).
- Format: 16-bit signed interleaved stereo PCM (little-endian), 4 bytes per frame.
- Supported hardware sample rates: 44,100 Hz and 48,000 Hz.
- I/O: Blocking `write()` calls to `/dev/eac`. Ring buffer capacity is set to 8,192 bytes (2,048 stereo frames, ~46.4ms latency at 44.1 kHz).
- Audio thread priority: `SCHED_FIFO` with real-time priority 80.

### 2.2 Audio Pop & Click Elimination
Testing revealed distinct acoustic pops during track changes, sample rate changes, and volume adjustments.

#### Root Causes
1. **Clock Divider Discontinuities:** Switching PLL divisors between 44.1 kHz and 48.0 kHz while the MT6323 DAC reconstructor filter was active produced sharp DC transient spikes.
2. **Buffer Flush Truncation:** Terminating playback without zeroing DMA buffers left residual voltage offsets on the output stage prior to cutting amplifier power.
3. **FM Radio Coexistence:** The analog radio path shared amplifier bias lines without proper isolation during digital audio state changes.

#### Mitigations
- **Settling Ramp:** When stopping or switching tracks, the driver feeds 256 samples of digital silence ($0x0000$) to `/dev/eac` before closing the file descriptor or issuing sample rate IOCTLs.
- **Hardware Mute Sequencing:** The analog headphone amplifier line is muted before PLL reconfiguration and unmuted only after valid audio frames are queued in DMA.
- **Gain Staging:** Digital attenuation handles all volume reduction down to -74 dBFS with soft block-level dithered interpolation. Analog gain on the MT6323 is held within 0 dB to +12 dB headroom, preventing digital clipping.

---

## 3. Power Management & Standby Current

### 3.1 Problem: High Idle Battery Drain
The device drained 100% battery in 9 hours while idle (screen off, music stopped, diagnostics disabled).

### 3.2 Root Cause Analysis
1. **CPU Frequency:** CPU 0 remained locked at 1196 MHz or scaled erratically because default governors did not enter lower operating points on display sleep.
2. **Secondary Core Leakage:** Cortex-A7 Core 1 remained in WFI (Wait-For-Interrupt) rather than being completely power-gated via CPU hotplug.
3. **SPM Deep Sleep Blocked:** The MediaTek System Power Manager (SPM) was prevented from entering regular idle sleep states by held driver wakelocks and peripheral clock enables.
4. **Quiescent Current in Audio Stage:** The MT6323 charge pump and headphone amplifier bias remained energized when audio output was idle.
5. **Storage Writeback Timers:** Default 5-second VM dirty writeback forced regular eMMC/SD bus wakeups.

### 3.3 Mitigations
- **Screen-Off Frequency Scaling:**
  - On backlight off, CPU 0 is dropped to 598 MHz via `/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed`.
  - CPU 1 is offlined via `echo 0 > /sys/devices/system/cpu/cpu1/online`, power-gating the entire secondary core.
- **Zero Quiescent Audio Shutdown:**
  - Closing `/dev/eac` powers down the analog headphone amplifier and DAC bias lines.
- **VM Flush Tuning:**
  - Extended `/proc/sys/vm/dirty_writeback_centisecs` to 12000 (120 seconds).
  - Runtime PM enabled across MMC host controllers.
- **Result:**
  - Standby current dropped from ~120 mA to < 5 mA.
  - Active playback battery life reaches ~50 hours.

---

## 4. Boot-Time Optimization

### 4.1 Latency Profile

| Phase | Unoptimized | Optimized | Delta |
| :--- | :--- | :--- | :--- |
| Linux Kernel Initialization | 2200 ms | 2200 ms | 0 ms |
| initramfs: CPU Governor / Setup | 350 ms | 50 ms | -300 ms |
| initramfs: MicroSD Detection | 1100 ms | 280 ms | -820 ms |
| initramfs: Framebuffer Splash & Links | 420 ms | 80 ms | -340 ms |
| Rockbox Binary Relocation | 850 ms | 420 ms | -430 ms |
| Rockbox Core & UI Initialization | 1450 ms | 850 ms | -600 ms |
| **Total Cold Boot** | **6370 ms** | **3880 ms** | **-2490 ms (~40% faster)** |

### 4.2 Implementation
1. **Early Boot CPU Boost:**
   - In `initramfs/init.c`, `boost_cpu_for_boot()` brings up both Cortex-A7 cores at 1.2 GHz performance governor before mounting filesystems.
   - Boost remains locked until Rockbox calls `y1_boot_boost_finish()` after UI setup, transitioning to the user-configured CPU profile.
2. **Reactive MicroSD Polling:**
   - Replaced 100ms static sleeps with 10ms reactive polling against `/proc/partitions`.
   - Card detection occurs within 10-30ms of driver registration.
3. **Framebuffer & Symlink Deduplication:**
   - State tracking (`current_displayed_screen`) avoids redundant RAW screen writes.
   - `link_sdcard_to_root()` inspects `readlink()` to skip existing links.
4. **Boot Splash Continuity:**
   - `show_logo_boot()` in `apps/main.c` calls `y1_display_raw_screen("boot")`, preventing the default Rockbox logo from overwriting the boot splash before the UI is ready.

---

## 5. Input System & Action Mapping

### 5.1 Event Nodes & Button Codes
Inputs are mapped from Linux input events:
- Center: `KEY_ENTER` / `BTN_SELECT`
- Top (Menu): `KEY_BACK` / `KEY_ESC`
- Bottom (Play/Pause): `KEY_PLAYPAUSE`
- Left (Prev): `KEY_PREVIOUSSONG` / `KEY_REWIND`
- Right (Next): `KEY_NEXTSONG` / `KEY_FASTFORWARD`
- Side Buttons: `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`
- Headphone Remote: `KEY_MEDIA` / `BTN_0` via `accdet`

### 5.2 Previous / Rewind Consistency Bug Fix
- **Issue:** In Advanced Key Lock and via the MFB remote, pressing Previous skipped immediately to the previous track regardless of playback progress. Standard Rockbox WPS behavior rewinds to the track/cue start if elapsed time is $\ge 3\text{ seconds}$, skipping to the previous track only if $< 3\text{ seconds}$. Seeking in Key Lock also emitted premature skip events.
- **Fix:** In `apps/misc.c`, `BUTTON_MULTIMEDIA_PREV` checks `id3->elapsed >= DEFAULT_SKIP_THRESH` (3000ms). In `apps/action.c`, Key Lock button press events swallow the initial down state during seek and dispatch native `ACTION_WPS_SEEK*` actions so the WPS status displays correctly.

---

## 6. USB Gadget & Charge-Only Switching

- Mass Storage: Managed by the Linux USB gadget subsystem exposing `/dev/block/mmcblk1` (or `mmcblk1p1`) via `/sys/class/android_usb/android0/f_mass_storage/`.
- Charge-Only Mode: In `apps/gui/usb_screen.c`, holding the Center button for 1 second (`BUTTON_SELECT` with repeat duration $\ge 1000\text{ ms}$) tears down the USB mass storage gadget and remounts the SD card read-write for Rockbox, allowing playback while charging.
- Host PC Ejection & Re-Attachment Fix:
  - **Problem:** When ejecting the device from a host PC (SCSI `START_STOP` command with LoEj bit set), the downstream Linux kernel `f_mass_storage` driver closed the backing file (`fsg_lun_close`), clearing `lun0/file` to an empty string. Subsequent connections or reconnects left `lun0/file` empty, presenting a generic empty card reader to Windows ("Please insert a disk into USB Drive").
  - **Fix:** In `firmware/target/hosted/innioasis/y1/usb-y1.c`, `y1_usb_attach_storage()` dynamically probes `/dev/block/mmcblk1` and binds it to `lun0/file` whenever entering USB slave mode (`usb_enable(true)` / `disk_unmount_all()`), re-enabling the gadget. In `apps/gui/usb_screen.c`, `y1_usb_check_host_ejected()` monitors `lun0/file`; when the host PC safely ejects the volume, the device immediately transitions to charge-only mode, unbinds the gadget, and returns to Rockbox playback.

---

## 7. Proprietary MediaTek Files Exclusion

To maintain open-source compliance:
- Proprietary blobs (`bt_blobs/`, `stock_unpacked/`, `logo.bin`, and all `*.ko` kernel objects) are excluded from Git via `.gitignore`.
- Custom code, reverse-engineered utilities (`unmute_eac.c`, `mkbootimg_mtk.py`), scripts, and Rockbox target drivers remain tracked.
- The build orchestrator (`build.py` / `build.sh`) unpacks required kernel binaries from `!stock_rom/boot.img` dynamically when building.

---

## 8. Sample Rate Switching & Phase Alignment Degradation Bug

### 8.1 Symptom
Rapid or repeated switching between audio tracks with differing sample rates (e.g. 44.1 kHz $\leftrightarrow$ 48.0 kHz) caused audible sound degradation: the presentation grew increasingly muddy, vocal definition in the center vanished, and the soundstage receded into the distance without a drop in overall RMS playback volume.

### 8.2 Root Cause
1. **Stereo Frame Misalignment in Partial Writes:**
   The `pcm_worker` write loop previously rounded partial byte counts down using `written &= ~3`. The kernel circular buffer kept the unaligned count while user-space advanced by the rounded count. Subsequent writes started at an odd 2-byte boundary, inverting Left and Right stereo channels and shifting one channel by 1 sample ($d \approx 22.7\,\mu\text{s}$). The resulting inter-channel phase shift produced severe center cancellation and comb filtering.
2. **PMIC FIR Interpolator State Corruption:**
   Writing new clock divisors to PMIC NewIF (`0x4024`) and PMIC Codec DL (`0x4002`) while PMIC Downlink (`0x4014`) and Master (`0x4000`) were active corrupted the internal FIR digital filter state in the MT6323.
3. **Stuck PMIC CDC Sync Trigger:**
   In `top_ctl_trigger()`, the expression `toggle = ((~val) << 8) & 0x300` failed to toggle bits 8 and 9 because bits 0 and 1 of `0x4016` read zero, permanently disabling CDC clock resynchronization after boot.
4. **Kernel Ring Buffer Stale Frame Bleed:**
   Changing clock frequencies without resetting the 32 KB coherent DL1 buffer caused residual audio frames from the previous track to clock out at the new rate from unaligned offsets.

### 8.3 Fix Details
- **Frame Alignment Guarantee:** When `written & 3 != 0`, zero padding (`4 - unaligned` bytes) is written directly to `/dev/eac` to restore ring buffer alignment. User buffer pointers advance by `written + pad_len` to keep all subsequent frames 4-byte aligned.
- **PMIC Gating & FIR Reset:** `sink_set_freq()` disables `0x4014` and `0x4000` prior to modifying rate registers, resets PMIC digital gains (`0x4006`, `0x4008`), and re-enables master control cleanly.
- **DL1 Ring Buffer Reallocation:** `AUDDRV_FREE_DL1_BUF`, `AUDDRV_ALLOCATE_DL1_BUF`, and `SET_MEMIF_FP` reset kernel read and write pointers to 0 on sample rate changes.
- **CDC Trigger XOR Toggle:** `top_ctl_trigger()` uses `(val ^ 0x300) & 0x300` in `pcm-eac.c` and `unmute_eac.c`.
- **Concurrency Protection:** `sink_set_freq()` is protected with `sink_lock()` / `sink_unlock()` and clears `pcm_data` under `pcm_mtx`.

