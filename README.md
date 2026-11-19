<img width="1440" height="1080" alt="hero1" src="https://github.com/user-attachments/assets/10e1d348-e75f-4271-a893-6ff63120375e" />

🌐 [한국어](krREADME.md) | **English**

# Nugget for Innioasis Y1

[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#hardware-specifications)

Nugget for Y1 is an open-source custom firmware for the Innioasis Y1 digital audio player. It replaces Android entirely with a minimal downstream Linux kernel (3.4.67) booting directly into native Hosted Rockbox.

---

## Technical Comparison

| Area | Stock Android | Rockbox on Android | Nugget for Y1 (Hosted Linux) |
| :--- | :--- | :--- | :--- |
| OS / Runtime | Android (MediaTek MT6572 userland, Zygote/Dalvik) | Rockbox APK running inside Android userland | Minimal Linux initramfs (kernel 3.4.67) booting directly into Hosted Rockbox (`PID 1 /init`) |
| Audio Output Path | Android AudioFlinger -> Android audio HAL -> `/dev/eac` | Android AudioFlinger (`AudioTrack`/OpenSL) -> Android audio HAL -> `/dev/eac` | Direct `/dev/eac` writes from `pcm-eac.c` (bypasses AudioFlinger) |
| Sample Rate Handling | Resampled/mixed by AudioFlinger | Resampled/mixed by AudioFlinger | Direct 44.1 kHz and 48 kHz hardware rate switching |
| Volume / Gain Control | Android audio stream steps + MT6323 analog PGA | Rockbox software volume + Android media stream | Rockbox logarithmic volume mapped to PCM attenuation + MT6323 analog PGA (`0x70a`) |
| Bluetooth Audio | Android Bluetooth stack (Bluedroid) | Android Bluetooth stack (Bluedroid) | MediaTek `stp_uart_launcher` + `mtk_wmt_bt` + BlueZ/A2DP (SBC) |
| Power / Suspend | Android `earlysuspend` + `wakelock` (`mem` suspend) | Android `earlysuspend` + `wakelock` (`mem` suspend) | Direct `wakelock` / `earlysuspend` + `/sys/power/state` (`mem` suspend) + CPU hotplug/governor control |
| Display Resolution | 480x360 | Depends on APK build (typically 480x360) | 480x360 native with runtime 320x240 scaling mode |
| Key Lock (HOLD) | None | Rockbox key lock | Rockbox key lock with double-click MENU toggle, Hold Light, lock backlight rules, and selective key exemptions |
| USB Modes | MTP / Android USB Mass Storage | MTP / Android USB Mass Storage (via OS) | USB Mass Storage and optional Apple iPod 5.5G emulation (`ipod_vpd.ko`, disabled by default) |
| iTunes Integration | None | None | Optional `iTunesDB` / `ArtworkDB` / `.ithmb` TagCache & playlist parsing, `Play Counts` / `OTGPlaylistInfo` writing |
| Dual Boot | Stock OS only | Android app | Pre-boot menu supporting Rockbox, MeUI, custom user binary, and stock Android handoff |

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

### Recommended: Nugget Y1 Tool
Automated installation tool:
https://github.com/norocoral/nugget-y1-tool

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

### Build Command
```bash
./build.sh
```

The build script will:
1. Extract vendor kernel from `!stock_rom/boot.img` if not already present.
2. Compile custom PID 1 C `/init` and `unmute_eac` utilities with ARM hard-float GCC.
3. Convert splash screen PNG assets into raw 32-bit framebuffer files.
4. Compile Hosted Rockbox for the Innioasis Y1 (`build_y1/`).
5. Package `.rockbox` SD card assets into `rockbox_sdcard.zip`.
6. Assemble minimal initramfs ramdisk and package flashable `boot_rockbox.img` with MediaTek headers.

*Note: Proprietary MediaTek binaries (`bt_blobs/`, `stock_unpacked/`, `logo.bin`) are excluded from Git to maintain open-source compliance.*

---

## Button Controls

Same navigation scheme as the iPod Classic, with dedicated hardware additions:

| Control | Action | Function |
| :--- | :--- | :--- |
| Click Wheel | Rotate | Scroll lists / adjust volume in WPS |
| Center Button | Short Press | Select / activate menu item |
| Center Button | Long Press (1s) | Open Context Menu for active track/file |
| Center Button | Long Press (1s on USB screen) | Eject Mass Storage and switch to Charge-Only mode |
| Menu Button | Short Press | Return to parent menu / back (120ms release threshold) |
| Menu Button | Double-Click | Toggle Key Lock (HOLD mode) on / off |
| Menu Button | Long Press (280ms) | Open Rockbox Quick Screen settings |
| Play Button | Short Press | Play / Pause toggle |
| Play Button | Long Press | Stop playback |
| Left Button | Short Press | Rewind to track start if $\ge 3\text{s}$; skip to previous track if $< 3\text{s}$ |
| Left Button | Long Press | Seek backward |
| Right Button | Short Press | Skip to next track |
| Right Button | Long Press | Seek forward |
| Headphone Remote (MFB) | In-line Clicks | Single, double, triple clicks & hold actions (configurable in Settings) |

---

### HOLD & Backlight Subsystem

- **Toggling HOLD (Lock / Unlock):**
  - Double-clicking **MENU** toggles HOLD mode.
  - Double-click detection uses a 350ms window when locked; single-click MENU fires after a 120ms window when unlocked.
- **When Locked (HOLD Active):**
  - **Button Protection:** Wheel and button inputs are blocked unless exempted via *Advanced Key Lock* (Volume, Play/Pause, or Track Skip).
  - **Hold Light Setting:**
    - `Off` *(Default)*: Button presses and wheel movement are ignored; the display remains off.
    - `On` *(or Brightness %)*: Pressing a button wakes the backlight without triggering playback commands.
  - **Backlight on Lock Timeout:** Configurable up to 5 hours (`Off`, `Always On`, `1s`–`45s`, `1m`–`45m`, `1h`–`5h`).
  - **Backlight on Lock Rules:** `Always`, `Playing only`, or `WPS only`.
- **When Unlocked (Normal Operation):**
  - **First Keypress Enables Backlight Only:**
    - `No` *(Default)*: Pressing a button while the backlight is off executes the command and turns on the backlight.
    - `Yes`: The first button press while dark only turns on the backlight; subsequent presses execute commands.
  - **Backlight Idle Timeout:** Controlled by the standard Rockbox Backlight Timeout setting.

---

### Dual-Boot Subsystem & Bootloader Menu

Android partitions on the internal eMMC (`SYSTEM`, `DATA`, `CACHE`) are left unmodified, allowing dual-booting alongside Android:

- **Bootloader Menu:**
  - Runs at startup before Rockbox is launched.
  - Available entries:
    - **Pick 1: Rockbox** — Boots Rockbox from the MicroSD card.
    - **Pick 2: MeUI** — Launches MeUI when `.meui/meui` is present.
    - **Pick 3: User Software** — Launches a user binary when `.custom/launch` is present.
    - **Pick 4: Android** — Clears BCB flags, unpacks the stock ramdisk, mounts `SYSTEM`/`DATA`/`CACHE`, and execs Android `init` as PID 1.
    - **Pick 5: Shut down** — Powers off the device.
    - **Pick 6: USB Mass Storage** — Enables USB Mass Storage from the bootloader; pressing any button exits USB mode and returns to the menu.
    - **Pick 7: Linux terminal (USB)** — Starts USB serial root shells on `/dev/ttyGS0` and `/dev/ttyGS1`.
  - **Navigation:** Click Wheel or **Left / Right** selects an entry; **Center** or **Play** confirms.
  - **Hardware Override:** Holding **MENU** during boot forces the Bootloader Menu to appear regardless of settings.
  - **Settings Toggle:** `Settings -> Nugget -> Bootloader Menu` (`Yes` / `No`).
- **Reboot to Android from Rockbox:**
  - `Settings -> Nugget -> Reboot to Android` flushes buffers and hands off to stock Android.

---

## Credits
https://github.com/nuxcodes/rockpod

---

## License

Built on the open-source [Rockbox](https://www.rockbox.org) project under the GNU General Public License v2 (GPL-2.0).
Not affiliated with Innioasis.

## Transparency
The reverse engineering, drivers, and boot code in this repository were generated with AI assistance (Google Antigravity).