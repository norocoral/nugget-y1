# Hosted Rockbox on Minimal Linux (Innioasis Y1)

This directory contains the build system, minimal Linux initramfs, hardware abstraction drivers, and packaging tools for the Innioasis Y1 Rockbox port. The flashable boot image contains the Linux kernel and early startup logic; the Rockbox executable and system assets reside on the external MicroSD card.

---

## Technical Documentation

Detailed subsystem specifications and reverse engineering notes are located in [`docs/`](docs/):
- [Technical Findings](docs/TECHNICAL_FINDINGS.md): Reverse-engineered hardware interfaces, EAC audio driver, PMIC registers, power tuning, input quirks, and boot latency analysis.
- [System Architecture](docs/ARCHITECTURE.md): Component diagrams, boot sequence flow, storage hierarchy, and HAL mapping.
- [Audio Subsystem](docs/AUDIO_SUBSYSTEM.md): `/dev/eac` interface, bit-perfect 44.1/48kHz playback, pop elimination, and gain staging.
- [Power Management](docs/POWER_MANAGEMENT.md): Idle battery drain fixes, CPU frequency governors, core offlining, and Always-On Display.

---

## Directory Structure

- `docs/`: Technical specifications, architecture designs, and hardware findings.
- `initramfs/`: PID 1 `/init` in C. Handles boot CPU boosting, reactive MicroSD discovery, framebuffer status screens, and handoff to Rockbox.
- `audio/`: Standalone `unmute_eac.c` test utility and `init_audio.sh` setup script.
- `splash/`: Source PNG status screens (`boot.png`, `connect.png`, `errconnect.png`, `error.png`, `sdcard.png`).
- `Themify2/`: Pre-bundled custom Rockbox theme, fonts, icons, and codecs for the SD card distribution.
- `tools/`: Build scripts, splash image converter (`convert_splash.py`), MTK boot image packer (`mkbootimg_mtk.py`), and CPIO ramdisk generator (`build_initramfs.py`).
- `rootfs/`: (Generated during build) Skeleton for the minimal initramfs ramdisk.
- `stock_unpacked/`: (Generated during build) Extracted vendor kernel (`kernel_mtk.bin`), excluded from Git tracking.
- `bt_blobs/`: MediaTek Bluetooth kernel modules and firmware patches, excluded from Git tracking.

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

### Build Process
1. **Kernel Extraction:** If `stock_unpacked/kernel_mtk.bin` is missing, `build.sh` locates `!stock_rom/boot.img` and extracts the vendor kernel using `tools/mkbootimg_mtk.py`.
2. **PID 1 Compilation:** Compiles `initramfs/init.c` with `arm-linux-gnueabihf-gcc` and copies minimal shared libraries into `rootfs/lib/`.
3. **Splash Assets:** Converts PNG images in `splash/` into raw 32-bit framebuffer files.
4. **Rockbox Compilation:** Builds the Rockbox executable targeting the Innioasis Y1 (`build_y1/`).
5. **SD Payload Assembly:** Packages `.rockbox/rockbox`, codecs, fonts, and themes into `rockbox_sdcard.zip`.
6. **Boot Image Packaging:** Assembles `ramdisk_rockbox.img` (gzip + MTK header) and packages `boot_rockbox.img` with MTK boot headers (max 6 MiB).

---

## Proprietary Blob Handling

To comply with open-source licensing:
- MediaTek proprietary binaries (`bt_blobs/`, `stock_unpacked/`, `logo.bin`, and all `*.ko` kernel objects) are excluded from Git via `.gitignore`.
- All custom C code, reverse-engineered tools, Python build scripts, and Rockbox port drivers remain tracked.

---

## Diagnostic Logging

Y1 diagnostic logging is disabled by default. Enable **Settings -> Nugget -> Device -> Diagnostic Logging** to append init, display, audio, Bluetooth, button, and suspend diagnostics to `/mnt/sdcard/y1_debug.txt`. The setting is stored in `.rockbox/config.cfg`; `/init` reads it at boot to decide whether to output startup diagnostics.

---

## Hardware Controls

| Control | Action | Function |
| :--- | :--- | :--- |
| Click Wheel | Rotate | Scroll lists / adjust volume in WPS |
| Center Button | Short Press | Select / activate menu item |
| Center Button | Long Press (1s) | Open Context Menu for active track/file |
| Center Button | Short Press (Screen Off) | Unhold and wake screen (click is cleanly swallowed) |
| Center Button | Long Press (1s on USB screen) | Eject Mass Storage and switch to Charge-Only mode |
| Menu Button | Short Press | Return to parent menu / back |
| Menu Button | Long Press | Open Rockbox Quick Screen settings |
| Play Button | Short Press | Play / Pause toggle |
| Play Button | Long Press | Stop playback |
| Left Button | Short Press | Rewind to track start if $\ge 3\text{s}$; skip to previous track if $< 3\text{s}$ |
| Left Button | Long Press | Seek backward |
| Right Button | Short Press | Skip to next track |
| Right Button | Long Press | Seek forward |
| Side Buttons | Press | Volume Up / Down (1 dB steps) |
| Headphone Remote (MFB) | In-line Clicks | Single, double, triple clicks & hold actions (configurable in Settings) |

## Diagnostic Logging

Y1 diagnostic logging is disabled by default. Enable **Settings -> Nugget -> Device -> Diagnostic Logging** to append init, display, audio, Bluetooth, button, and suspend diagnostics to `/mnt/sdcard/y1_debug.txt`. The setting is stored in `.rockbox/config.cfg`; `/init` reads it at boot to decide whether to output startup diagnostics.

---

## Hardware Controls

| Control | Action | Function |
| :--- | :--- | :--- |
| Click Wheel | Rotate | Scroll lists / adjust volume in WPS |
| Center Button | Short Press | Select / activate menu item |
| Center Button | Long Press (1s) | Open Context Menu for active track/file |
| Center Button | Short Press (Screen Off) | Unhold and wake screen (click is cleanly swallowed) |
| Center Button | Long Press (1s on USB screen) | Eject Mass Storage and switch to Charge-Only mode |
| Menu Button | Short Press | Return to parent menu / back |
| Menu Button | Long Press | Open Rockbox Quick Screen settings |
| Play Button | Short Press | Play / Pause toggle |
| Play Button | Long Press | Stop playback |
| Left Button | Short Press | Rewind to track start if $\ge 3\text{s}$; skip to previous track if $< 3\text{s}$ |
| Left Button | Long Press | Seek backward |
| Right Button | Short Press | Skip to next track |
| Right Button | Long Press | Seek forward |
| Side Buttons | Press | Volume Up / Down (1 dB steps) |
| Headphone Remote (MFB) | In-line Clicks | Single, double, triple clicks & hold actions (configurable in Settings) |

