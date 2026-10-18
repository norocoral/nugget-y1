# Nugget for Innioasis Y1 (Hosted Rockbox on Minimal Linux)

[![Release](https://img.shields.io/badge/Release-v1.0.0-brightgreen.svg)](hosted_linux_port/release)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#hardware-specifications)
[![Headroom](https://img.shields.io/badge/BOOTIMG%20Headroom-196%20KB-success.svg)](#build-specifications)

**Nugget for Y1** is an open-source custom firmware (CFW) for the **Innioasis Y1** digital audio player. It eliminates Android completely, booting a minimal downstream Linux kernel directly into native **Hosted Rockbox**.

---

## Why Nugget for Y1?

| Feature | Stock Android | Rockbox on Android | Nugget for Y1 (Linux) |
| :--- | :---: | :---: | :---: |
| **Battery Life** | ~30 Hours | ~20 Hours | **~50 Hours (2.5x)** |
| **Boot Time** | ~40 Seconds | ~45 Seconds | **< 10 Seconds** |
| **Headphone Amp Power** | Full | Reduced / Software limited | **Full (+12 dB analog PGA)** |
| **Sample Rate Switching** | Android AudioFlinger resampled | Resampled to 44.1 kHz | **Native bit-perfect 44.1 & 48 kHz** |
| **Volume Scaling** | Android steps | Broken / jumpy | **Authentic Rockbox dB scale (-74 to +6 dB)** |
| **Screen Resolutions** | 480x360 only | 480x360 only | **480x360 Native + 320x240 iPod Classic Mode** |
| **iPod Theme Support** | Stretched / Distorted | Requires 480x360 themes | **Full iPod Classic themes supported natively** |
| **HOLD Switch Mode** | None (Soft lock only) | None | **Automatic HOLD on screen timeout, Center to unhold** |
| **USB Music Transfer** | MTP (Slow) | MTP | **USB Mass Storage (Standard USB Drive)** |

---

## Key Features

1. **2.5x Battery Life:** Without the background Android OS (Dalvik VM, Stagefright, Binder, SystemServer), the dual-core Cortex-A7 runs cool in deep idle at 598MHz, giving up to ~50 hours of audio playback.
2. **Lossless Audio Engine:**
   - Direct kernel PCM streaming via MediaTek `/dev/eac`.
   - Real-time audio priority (`SCHED_FIFO` 80) and 4-byte frame alignment preventing audio underruns, jitter, and digital distortion.
   - Dynamic sample rate switching: 44.1 kHz and 48 kHz tracks play at true native clock speeds.
   - Genuine logarithmic volume curve (-74 dB to +6 dB in 1 dB increments) with digital clipping protection ($\le 0\text{ dB}$).
3. **Dual Resolution Display Driver:**
   - **Normal (480x360):** 1:1 native hardware framebuffer.
   - **Classic (320x240):** Nearest-neighbor full-screen scaling enabling the entire library of 320x240 iPod Classic Rockbox themes.
   - Change in `Settings -> General Settings -> System -> Screen Resolution` (applies on next manual restart).
4. **Automatic HOLD Mode & Advanced Key Lock:**
   - **Auto-Hold:** Whenever the backlight turns off (via timeout or pressing the Power button), the device automatically enters HOLD mode.
   - **Advanced Key Lock:** Settings in `Settings -> General Settings -> System -> Advanced Key Lock` are applied while the screen is off (e.g. keep Volume or Play/Pause buttons active in your pocket). All other buttons are silently blocked without waking the screen.
   - **Instant Unhold:** Simply press the **Center button** to unhold and wake up the screen. The wake press is cleanly swallowed to avoid unintended clicks.
   - **Context Menu:** Long-press Center (~1s) opens the track/file Context Menu when awake.
5. **MicroSD Storage:**
   - Supports FAT32, exFAT (SDXC 256GB+), NTFS, and ext4.
   - Multi-partition support: Partition 1 mounted at `/mnt/sdcard` (`/`), Partition 2 at `/mnt/extsd` (`<microSD1>`).
   - Drag-and-drop music and theme management via built-in USB Mass Storage.
6. **Hardware Status Screens:**
   - Custom graphical framebuffer screens for `boot`, missing `sdcard`, PC `connect`, `errconnect`, and `error`.

---

## Hardware Specifications

- **SoC:** MediaTek MT6572 (2x Cortex-A7 @ 598 MHz – 1.3 GHz, NEON, VFPv4)
- **RAM:** 512 MB LPDDR2
- **Display:** 480x360 Landscape LCD, 32-bit ARGB8888 hardware FB (`/dev/graphics/fb0`)
- **Keypad:** Synaptics/Focaltech I2C touch wheel & buttons + physical center switch (`/dev/input/event0..event15`)
- **Audio Codec / Amp:** MT6323 PMIC DAC & Headphone Amp via `/dev/eac`
- **Partition Limit:** `BOOTIMG` partition = 6,291,456 bytes (6.00 MB)
- **Boot Image Size:** 6,094,848 bytes (Fits with 196,608 bytes headroom)

---

## Quick Install Guide

### 1. Download Release
Grab the latest release files from [`hosted_linux_port/release/`](hosted_linux_port/release/):
- `boot_rockbox.img`
- `rockbox_sdcard.zip`

### 2. Flash Boot Partition
Flash `boot_rockbox.img` to the `boot` partition using:
- **Innioasis Updater CE:** Select *Nugget for Y1* and follow the prompt.
- **SP Flash Tool:** Load MT6572 scatter, check `BOOTIMG`, select `boot_rockbox.img`, click Download, connect Y1 holding Volume Down.
- **Fastboot / python-mtk:**
  ```bash
  python mtk.py w boot boot_rockbox.img
  ```

### 3. Setup MicroSD Card
1. Format your card as **FAT32** or **exFAT**.
2. Extract `rockbox_sdcard.zip` to the root of the card so the `.rockbox` directory sits at the top level.
3. Insert the card into your Y1 and power on!

---

## Building from Source

### Prerequisites (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y build-essential gcc-arm-linux-gnueabihf python3 python3-pip zip curl
```

### Build Command
```bash
cd hosted_linux_port
./build.sh
```
The build script will:
1. Compile the custom PID 1 C `/init` and `unmute_eac` utilities.
2. Convert graphical boot/alert PNG assets into raw 32-bit framebuffer files.
3. Configure and compile Rockbox (`arm-linux-gnueabihf-gcc`).
4. Package `.rockbox` SD card assets into `rockbox_sdcard.zip`.
5. Assemble the initramfs ramdisk and package `boot_rockbox.img` with MediaTek headers.

---

## Button Controls

| Action | Button |
| :--- | :--- |
| **Scroll Lists / Volume** | Click Wheel (Clockwise / Counter-Clockwise) |
| **Select / Open** | Center Button (Short Press) |
| **Context Menu** | Center Button (Hold ~1s) |
| **HOLD Mode (Lock)** | Center Button (Hold 5s) |
| **UNHOLD Mode (Unlock)** | Center (Hold 5s) $\to$ Release $\to$ Click Center |
| **Screen Wakeup** | Any Button (Turns screen ON without activating action) |
| **Back / Menu** | Top Menu Button (Short Press) |
| **Quick Screen** | Top Menu Button (Long Press) |
| **Play / Pause** | Bottom Play Button (Short Press) |
| **Stop** | Bottom Play Button (Long Press) |
| **Prev / Seek Back** | Left Button (Short / Long Press) |
| **Next / Seek Forward**| Right Button (Short / Long Press) |
| **Volume Up / Down** | Side Volume Buttons |

---

## License

Built on the open-source [Rockbox](https://www.rockbox.org) project under the GNU General Public License v2 (GPL-2.0).
Not affiliated with Innioasis.

## Transparancy
I believe that code practices should be transparent.
The reverse engineering, drivers and boot code is generated by AI (Google Antigravity).