<img width="1440" height="1080" alt="hero1" src="https://github.com/user-attachments/assets/10e1d348-e75f-4271-a893-6ff63120375e" />

🌐 [한국어](krREADME.md) | **English**

# Nugget for Innioasis Y1

[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#hardware-specifications)

Nugget for Y1 is an open-source custom firmware for the Innioasis Y1 digital audio player. It replaces Android entirely with a minimal downstream Linux kernel (3.4.67). It has a developer library, so developers can create their own experiences natively.

---

## Technical Comparison

| Area | Stock Android | Rockbox on Android | Nugget for Y1 (Hosted Linux) |
| :--- | :--- | :--- | :--- |
| OS / Runtime | Android (MediaTek MT6572 userland, Zygote/Dalvik) | Rockbox APK running inside Android userland | Minimal Linux initramfs (kernel 3.4.67) booting directly into Hosted Rockbox (`PID 1 /init`) |
| Audio Output Path | Android AudioFlinger -> Android audio HAL -> `/dev/eac` | Android AudioFlinger (`AudioTrack`/OpenSL) -> Android audio HAL -> `/dev/eac` | Direct `/dev/eac` writes from `pcm-eac.c` (bypasses AudioFlinger) |
| Sample Rate Handling | Resampled/mixed by AudioFlinger | Resampled/mixed by AudioFlinger | Direct 44.1 kHz and 48 kHz hardware rate switching |
| Volume / Gain Control | Android audio stream steps + MT6323 analog PGA | Rockbox software volume + Android media stream | Rockbox logarithmic volume mapped to PCM attenuation + MT6323 analog PGA (`0x70a`) |
| Bluetooth Audio | Android Bluetooth stack (Bluedroid) | Android Bluetooth stack (Bluedroid) | Direct `liby1` MT6572 CONNSYS (`/dev/stpwmt`, `/dev/stpbt`) + custom A2DP (SBC, SBC-XQ, AAC) & Receiver mode |
| Power / Suspend | Android `earlysuspend` + `wakelock` (`mem` suspend) | Android `earlysuspend` + `wakelock` (`mem` suspend) | Direct `wakelock` / `earlysuspend` + `/sys/power/state` (`mem` suspend) + CPU hotplug/governor control |
| Display Resolution | 480x360 | Depends on APK build (typically 480x360) | 480x360 native with runtime 320x240 scaling mode |
| Key Lock (HOLD) | None | Rockbox key lock | Rockbox key lock with double-click MENU toggle, Hold Light, lock backlight rules, and selective key exemptions |
| USB Modes | MTP / Android USB Mass Storage | MTP / Android USB Mass Storage (via OS) | USB Mass Storage and optional Apple iPod 5.5G emulation (`ipod_vpd.ko`, disabled by default) |
| iTunes Integration | None | None | Optional `iTunesDB` / `ArtworkDB` / `.ithmb` TagCache & playlist parsing, `Play Counts` / `OTGPlaylistInfo` writing |
| Dual Boot | Stock OS only | Android app | Pre-boot **ConfigApp** (`--bootmenu`) supporting Launch (hidden-folder auto-discovery), Android handoff, USB, Serial Terminal, Wi-Fi/BT, and SD-missing Text GUI |

---

## Technical Documentation

Subsystem specifications, architecture notes, and reverse-engineering documentation are located in [`docs/`](docs/):
- [iPod 5.5G USB Mode & iTunes Integration](docs/IPOD_SYNC.md): `ipod_vpd.ko` EVPD spoofing, `iTunesDB` TagCache loading, `.ithmb` artwork decoding, `Play Counts`/`OTGPlaylistInfo` writing, and USB gadget configuration.
- [Technical Findings](docs/TECHNICAL_FINDINGS.md): Hardware interfaces, `/dev/eac` audio driver, PMIC registers, power tuning, boot measurements, and input event behavior.
- [System Architecture](docs/ARCHITECTURE.md): Architecture block diagrams, boot sequence, storage layout, and HAL mapping.
- [Audio Subsystem](docs/AUDIO_SUBSYSTEM.md): MediaTek EAC driver, pop mitigation, clock switching, and gain staging.
- [Power Management](docs/POWER_MANAGEMENT.md): Suspend/wakelock handling, CPU frequency governors, core hotplugging, and Always-On Display (AOD).

---

## Installation

### Recommended: Innioasis Updater CE
https://innioasis.app/firmware_ko.html?brand=Innioasis&model=Y1&software=nugget-rock8ox-240p-360p-y1&type=A

### Manual Installation
1. Obtain `boot_rockbox.img` and `rockbox_sdcard.zip`.
2. Flash `boot_rockbox.img` to the `boot` partition using:
   - SP Flash Tool: Load MT6572 scatter, check `BOOTIMG`, select `boot_rockbox.img`, click Download, and connect Y1 holding Volume Down.
   - Fastboot / python-mtk:
     ```bash
     python mtk.py w boot boot_rockbox.img
     ```
3. Format MicroSD card as FAT32 or exFAT.
4. Extract `rockbox_sdcard.zip` directly to the card root so that `.rockbox` is located at the top level.
5. Insert card into the Y1 and power on.

---

## Building from Source

### Prerequisites (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y build-essential gcc-arm-linux-gnueabihf python3 python3-pip zip curl
```

### Required External Assets
1. **Stock `boot.img` (for kernel & Android dual-boot ramdisk):**
   Download the Y1 Type A stock ROM ([rom.zip](https://github.com/y1-community/y1-ata-rom/releases/download/0.1/rom.zip)), extract `boot.img`, and place it at `stock_rom/boot.img` (or set `STOCK_BOOT=/path/to/boot.img`).
2. **Rockbox Source (if building with Rockbox):**
   Clone `https://github.com/norocoral/rockbox` next to this repository (`../rockbox`) or set `ROCKBOX_SRC=/path/to/rockbox`. To build only the ROM/SDK without Rockbox, run `./build.sh --no-rockbox`.

### Build Command
```bash
./build.sh
```

The build script will:
1. Extract vendor kernel from `!stock_rom/boot.img` if not already present.
2. Compile custom PID 1 C `/init`, `unmute_eac`, `liby1`, and `configapp` with ARM hard-float GCC.
3. Convert `splash/bootorg.png` into raw 32-bit framebuffer splash assets using Wanted Sans with adaptive black/white contrast.
4. Compile Hosted Rockbox for the Innioasis Y1 (`build_y1/`).
5. Package `.rockbox` and `.system` SD card assets (including `configapp`, `bt_blobs/`, and `logo.bin` in `make_dist.sh` / scatter) into `rockbox_sdcard.zip`.
6. Assemble minimal initramfs ramdisk and package flashable `boot.img` / `boot_nugget.img` with MediaTek headers.

*Note: Proprietary MediaTek binaries (`bt_blobs/`, `stock_unpacked/`, `logo.bin`) are excluded from Git to maintain open-source compliance.*

---

### Dual-Boot Subsystem & ConfigApp Pre-Boot Menu

Android partitions on the internal eMMC (`SYSTEM`, `DATA`, `CACHE`) are left unmodified, allowing dual-booting alongside Android:

- **ConfigApp (`--bootmenu` & `Main Menu -> System -> System Settings`):**
  - Replaces the legacy Bootpick menu with a touch/wheel-friendly light-theme C UI (`/.system/configapp/configapp`, English/Korean switcher, 12-hour clock).
  - **Pages:**
    - **Launch** — Auto-discovers hidden folders on SD (`.` in folder name with an executable inside, e.g. `.rockbox`, `.meui`, `.custom`) plus Stock Android handoff.
    - **System Status**, **Display & Haptics**, **CPU & Power**, **Date & Time** (12h clock & RTC).
    - **Bluetooth** (`y1_bt_*`) & **Wi-Fi** (`y1_wifi_*` via `wlan_mt6582.ko` / `mtk_wmt_wifi.ko` / `/dev/wmtWifi`).
    - **USB & Storage** (USB Mass Storage, iPod 5.5G mode, performance tuning).
    - **Serial Terminal** (dual USB ACM root shells on `/dev/ttyGS0` and `/dev/ttyGS1`).
    - **Debug Info** (live hardware telemetry in ConfigApp UI + `debuginfo*.txt` export).
    - **System & Power** (ENG/KOR language, debug log) and **Exit / Shut Down** (Exit, Reboot, Shut Down).
  - **Missing SD Card Fallback:** If no MicroSD card is present at boot, PID 1 `/init` displays a basic 3-option Text GUI (**USB**, **Serial Console**, **Shut Down**).
  - **Hardware Override:** Holding **MENU** during boot forces ConfigApp (`--bootmenu`) to appear regardless of settings.
- **Rockbox Display & Sleep Settings:**
  - `Sleep (After Backlight Off)` (`HAVE_LCD_SLEEP_SETTING`) and `Sleep Backlight Off Brightness` are available in Rockbox display settings, alongside Lock Backlight Off support.

---

## Linked repositories 
Rockbox: https://github.com/norocoral/rockbox

---

## License

Not affiliated with Innioasis.

## Transparency
The reverse engineering, drivers, and boot code in this repository were generated with AI assistance (Google Antigravity).