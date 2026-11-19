🌐 [한국어](krTECHNICAL.md) | **English**

# Hosted Rockbox on Minimal Linux (Innioasis Y1)

This directory contains the decoupled build system, minimal Linux initramfs, standalone hardware abstraction layer (`liby1`), and packaging tools for the Innioasis Y1. The flashable boot image contains the Linux kernel and early startup logic; the Rockbox executable, hardware libraries, and system assets reside on the external MicroSD card.

---

## Technical Documentation & Guides

Detailed subsystem specifications, developer manuals, and reverse engineering notes:
- **[iPod 5.5G USB Mode & Sync Engine](docs/IPOD_SYNC.md)** ([한국어](docs/kr/IPOD_SYNC.md)): Complete guide to on-device bidirectional iTunes ↔ Rockbox sync, Apple 5.5G hardware spoofing, TagCache .tcd generation, .ithmb artwork extraction, video DB, and turbo lock.
- **[Modular Build System Guide](docs/BUILD_GUIDE.md)** ([한국어](docs/kr/BUILD_GUIDE.md)): Decoupled architecture, Terminal GUI (`--gui`), `build.conf` reference, CLI flags, standalone SDK builds, and flashing instructions.
- **[`liby1` Developer Manual & API Guide](docs/LIBY1_GUIDE.md)** ([한국어](docs/kr/LIBY1_GUIDE.md)): Complete C API documentation for display, audio, input, power, CPU, FM radio, and USB gadget.
- **[System Architecture](docs/ARCHITECTURE.md)** ([한국어](docs/kr/ARCHITECTURE.md)): Component diagrams, boot sequence flow, storage hierarchy (`.system` vs `.rockbox`), and HAL mapping.
- **[Technical Findings](docs/TECHNICAL_FINDINGS.md)** ([한국어](docs/kr/TECHNICAL_FINDINGS.md)): Reverse-engineered hardware interfaces, EAC audio driver, PMIC registers, power tuning, input quirks, and boot latency analysis.
- **[Audio Subsystem](docs/AUDIO_SUBSYSTEM.md)** ([한국어](docs/kr/AUDIO_SUBSYSTEM.md)): `/dev/eac` interface, bit-perfect 44.1/48kHz playback, pop elimination, and gain staging.
- **[Power Management](docs/POWER_MANAGEMENT.md)** ([한국어](docs/kr/POWER_MANAGEMENT.md)): Idle battery drain fixes, CPU frequency governors, core offlining, and Always-On Display.

---

## Directory Structure

- `docs/`: Technical specifications, architecture designs, build guides, and hardware findings.
- `liby1/`: Standalone C Hardware Abstraction Layer (`liby1.a`, `liby1.so`, `include/liby1.h`, example app).
- `initramfs/`: PID 1 `/init` in C. Handles boot CPU boosting, reactive MicroSD discovery, framebuffer status screens, Bootloader Menu, and handoff to Rockbox or custom app.
- `audio/`: Standalone `unmute_eac.c` test utility and `init_audio.sh` setup script.
- `splash/`: Source PNG status screens (`boot.png`, `connect.png`, `errconnect.png`, `error.png`, `sdcard.png`).
- `bootl_image_assets/`: Bootloader menu PNG icons (Rockbox, MeUI, User Software, Android, Shut Down, USB Mass Storage, Linux Terminal).
- `Themify2/`: Pre-bundled custom Rockbox theme, fonts, icons, and codecs for the SD card distribution.
- `tools/`: Modular build tools, Terminal GUI configurator (`build_tui.py`), splash image converter (`convert_splash.py`), MTK boot image packer (`mkbootimg_mtk.py`), and CPIO ramdisk generator (`build_initramfs.py`).
- `build.conf`: Central build configuration file.
- `rootfs/`: (Generated during build) Skeleton for the minimal initramfs ramdisk.
- `stock_unpacked/`: (Generated during build) Extracted vendor kernel (`kernel_mtk.bin`), excluded from Git tracking.
- `bt_blobs/`: MediaTek Bluetooth kernel modules and firmware patches, excluded from Git tracking.

---

## Building from Source

### Prerequisites (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y build-essential gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf python3 python3-pip zip curl ccache
```

### Interactive Terminal GUI (TUI)
You can configure build components visually using the built-in terminal menu:
```bash
./build.sh --gui
```
From the menu, use arrow keys and Space to toggle components (`liby1`, rootfs, boot image, Rockbox, SD packaging, custom app), or press `B` to save and build immediately.

### Command-Line Builds
```bash
# Standard full build (Rockbox + Boot Image + SD Card package)
./build.sh

# Standalone Linux SDK build (without Rockbox)
./build.sh --no-rockbox

# Build only liby1 (liby1.a, liby1.so, and hello_y1 example)
./build.sh --only-liby1

# Build only rootfs and boot_nugget.img
./build.sh --only-bootimg

# Deploy a custom standalone C binary to /.system/app
./build.sh --no-rockbox --custom-app /path/to/my_arm_app
```

---

## SD Card Storage Layout

All system, hardware, and developer files live in `/mnt/sdcard/.system/` (symlinked at `/.system`), keeping `/mnt/sdcard/.rockbox/` completely clean:

```text
/mnt/sdcard/
|-- .system/                      # System & hardware SDK files
|   |-- lib/                      # liby1.so, liby1video.so
|   |-- include/                  # liby1.h
|   |-- bin/                      # ffmpeg
|   |-- bt/                       # Bluetooth modules & firmware
|   |-- bt_paired.dat             # Paired devices database
|   |-- ramdisk_android.cpio.gz   # Stock Android ramdisk for dual-boot
|   |-- y1_debug.txt              # Hardware diagnostic log
|   `-- app                       # Optional standalone user executable
|-- .rockbox/                     # Rockbox binary, codecs, themes, fonts
`-- Videos/
```

---

## Diagnostic Logging

Y1 diagnostic logging is disabled by default. Enable **Settings -> Nugget -> Device -> Diagnostic Logging** to append init, display, audio, Bluetooth, button, and suspend diagnostics to `/mnt/sdcard/.system/y1_debug.txt`. The setting is stored in `.system/system.cfg` (or `.rockbox/config.cfg`); `/init` reads it at boot to decide whether to output startup diagnostics.

---

## Hardware Controls

| Control | Action | Function |
| :--- | :--- | :--- |
| Click Wheel | Rotate | Scroll lists / adjust volume in WPS |
| Center Button | Short Press | Select / activate menu item |
| Center Button | Long Press (1s) | Open Context Menu for active track/file |
| Center Button | Long Press (1s on USB screen) | Eject Mass Storage and switch to Charge-Only mode |
| Menu Button | Short Press | Return to parent menu / back (low-latency 120ms release) |
| Menu Button | Double-Click | Toggle Key Lock (HOLD mode) on / off |
| Menu Button | Long Press (280ms) | Open Rockbox Quick Screen settings |
| Menu Button | Hold while Powering On | **Force open Bootloader Menu** (bypasses timeout) |
| Play Button | Short Press | Play / Pause toggle |
| Play Button | Long Press | Stop playback |
| Left Button | Short Press | Rewind to track start if $\ge 3\text{s}$; skip to previous track if $< 3\text{s}$ |
| Left Button | Long Press | Seek backward |
| Right Button | Short Press | Skip to next track |
| Right Button | Long Press | Seek forward |
| Headphone Remote (MFB) | In-line Clicks | Single, double, triple clicks & hold actions (configurable in Settings) |

---

## iPod Classic HOLD & Backlight Subsystem

Nugget for Y1 replicates the classic iPod HOLD and backlight experience:

- **Toggling HOLD (Lock / Unlock):**
  - Rapidly double-click the **MENU** button at any time to toggle HOLD mode.
  - Double-click detection uses a 350ms window when locked and drops unhandled single clicks, preventing pocket wakes. When unlocked, single-click Menu has a swift 120ms release threshold for crisp menu navigation.
- **When Locked (HOLD Active):**
  - **Button Protection:** Blocks all wheel scrolls and buttons (unless allowed by *Advanced Key Lock*).
  - **Hold Light Setting:**
    - `Off` *(Default)*: Complete pocket blackout; all button presses are ignored without waking the screen.
    - `On` *(or Brightness %)*: Pressing any button wakes the backlight to view track/status without executing actions.
  - **Backlight on Lock Timeout:** Configurable up to **5 hours** (Off, Always On, 1s–45s, 1m–45m, 1h–5h). Applying lock immediately transitions to the lock timeout and brightness.
  - **Backlight on Lock Rules:** Select conditions under which the backlight remains active while locked (`Always`, `Playing only`, or `WPS only`).
- **When Unlocked (Normal Operation):**
  - **First Keypress Enables Backlight Only:**
    - `No` *(Default)*: Pressing a button when screen is off immediately performs the action (e.g. skip track, toggle pause) AND lights up the display simultaneously.
    - `Yes`: The first button press only wakes the display backlight (intercepting the action). Subsequent presses perform actions normally.
  - **Backlight Idle Timeout:** Controlled by the standard Backlight Timeout setting during UI browsing.
