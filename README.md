<img width="1440" height="1080" alt="hero1" src="https://github.com/user-attachments/assets/10e1d348-e75f-4271-a893-6ff63120375e" />

🌐 [한국어](krREADME.md) | **English**

# Nugget for Innioasis Y1

[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#hardware-specifications)

Nugget for Y1 is an open-source custom firmware for the Innioasis Y1 digital audio player. It replaces Android entirely with a minimal downstream Linux kernel (3.4.67) booting directly into native Hosted Rockbox.

---

## Technical Comparison

| Feature | Stock Android | Rockbox on Android | Nugget for Y1 (Hosted Linux) |
| :--- | :--- | :--- | :--- |
| Audio Playback Battery Life | ~30 Hours | ~20 Hours | **~50 Hours** |
| Standby Current | High (~120-140 mA) | High (~140-180 mA) | **< 5 mA (200+ hours)** |
| Cold Boot Time | ~40 Seconds | ~45 Seconds | **< 6 Seconds** |
| Sample Rate Support | Resampled via AudioFlinger | Resampled | **Native bit-perfect 44.1 & 48 kHz** |
| Audio Output Path | Standard Android HAL | Standard Android HAL | **Direct MT6572 /dev/eac** |
| Headphone Amp Gain | Full | Full | **Full (+12dB analog headroom)** |
| Volume Scale | OS master volume | Unstable with multi-output | **Rockbox logarithmic (-74 dB to +6 dB)** |
| Display Modes | 480x360 only | 480x360 only | **480x360 Native + 320x240 Classic Mode** |
| iPod Theme Compatibility | Incompatible | Requires 480x360 assets | **Native iPod Classic theme support** |
| Key Lock Mode | None | None | **Auto-HOLD with Advanced Key Lock** |
| USB Connection | Android MTP / Mass Storage | Android Mass Storage | **Mass Storage** |

---

## Technical Documentation

Detailed subsystem specifications, architecture diagrams, and reverse engineering findings are available in [`hosted_linux_port/docs/`](hosted_linux_port/docs/):
- [Technical Findings](hosted_linux_port/docs/TECHNICAL_FINDINGS.md): Hardware interfaces, `/dev/eac` audio driver, PMIC registers, power tuning, boot optimizations, and input event nuances.
- [System Architecture](hosted_linux_port/docs/ARCHITECTURE.md): Architecture block diagrams, boot sequence flow, storage layout, and HAL implementation map.
- [Audio Subsystem](hosted_linux_port/docs/AUDIO_SUBSYSTEM.md): MediaTek EAC driver, pop elimination mechanisms, bit-perfect clock switching, and gain staging.
- [Power Management](hosted_linux_port/docs/POWER_MANAGEMENT.md): Idle battery drain fixes (< 5mA standby), CPU frequency governors, core offlining, and Always-On Display (AOD).

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
cd hosted_linux_port
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

---

## Credits
https://github.com/nuxcodes/rockpod

---

## License

Built on the open-source [Rockbox](https://www.rockbox.org) project under the GNU General Public License v2 (GPL-2.0).
Not affiliated with Innioasis.

## Transparency
I believe that code practices should be transparent.
The reverse engineering, drivers and boot code is generated by AI (Google Antigravity).

The reverse engineering, drivers and boot code is generated by AI (Google Antigravity).