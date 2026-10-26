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

- Mass Storage: Managed by the Linux USB gadget subsystem exposing `/dev/block/mmcblk1p1`.
- Charge-Only Mode: In `apps/gui/usb_screen.c`, holding the Center button for 1 second (`BUTTON_SELECT` with repeat duration $\ge 1000\text{ ms}$) tears down the USB mass storage gadget and remounts the SD card read-write for Rockbox, allowing playback while charging.

---

## 7. Proprietary MediaTek Files Exclusion

To maintain open-source compliance:
- Proprietary blobs (`bt_blobs/`, `stock_unpacked/`, `logo.bin`, and all `*.ko` kernel objects) are excluded from Git via `.gitignore`.
- Custom code, reverse-engineered utilities (`unmute_eac.c`, `mkbootimg_mtk.py`), scripts, and Rockbox target drivers remain tracked.
- The build orchestrator (`build.py` / `build.sh`) unpacks required kernel binaries from `!stock_rom/boot.img` dynamically when building.

