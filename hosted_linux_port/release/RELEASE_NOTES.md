# Nugget for Innioasis Y1 - Release v1.0.0

**Nugget for Y1** is a custom firmware for the **Innioasis Y1** (MediaTek MT6572) that completely eliminates Android in favor of a lean, optimized downstream Linux kernel and native Hosted Rockbox.

---

## Highlights & Features

- 🔋 **2.5x Battery Life:** Up to ~50 hours of continuous audio playback (compared to ~20 hours on Rockbox-for-Android and ~30 hours on stock Android firmware).
- ⚡ **Instant Boot:** Powers on directly into Rockbox in under 10 seconds.
- 🎵 **High-Fidelity Lossless Audio:**
  - Direct hardware PCM sink via MT6572 `/dev/eac` and MT6323 PMIC DAC.
  - Dynamic sample rate switching: bit-perfect native playback for both **44.1 kHz** and **48 kHz** tracks without pitch or speed distortion.
  - Real-time audio thread priority (`SCHED_FIFO` 80) and 4-byte frame alignment preventing jitter, underruns, or channel phase slips.
  - Authentic Rockbox logarithmic volume scale (-74 dB to +6 dB in 1 dB steps) with digital clipping protection ($\le 0\text{ dB}$) and full +12dB hardware analog headphone amplifier gain.
- 🖥️ **Dual Screen Resolutions & Always-On Display:**
  - **Normal (480x360):** Native 1:1 hardware resolution.
  - **Classic (320x240 stretched):** Nearest-neighbor full-screen scaling for native iPod Classic Rockbox themes. Configurable in `Settings -> Nugget -> Device -> Screen Resolution` (applies on next manual reboot).
  - **Always On Display:** In `Settings -> Nugget -> Device -> Always On Display`. When enabled, dimming reduces backlight by 50% rather than turning it off, turns off the button LEDs, throttles hardware display refresh rate to 1 FPS, and puts the MTK display subsystem into power-saving mode (saving 97% of display bus bandwidth).
  - **AOD: On WPS Only:** (Default: ON) Only enables Always On Display when on the While Playing Screen (WPS). If navigating other menus or in file browser, screen powers off normally to conserve battery.
  - **AOD: When Playing Only:** (Default: OFF) Optional setting to only enable Always On Display while music is actively playing.
- 🎧 **Wired Headphone Inline Controls (MFB):**
  - Full support for 3.5mm headphone remote inline button (MFB / Headset Hook) even while the screen is off and device is in HOLD mode!
  - Fully customizable actions under `Settings -> Nugget -> Device -> Headphone Controls`:
    - **1 Click MFB** (Default: Play / Pause)
    - **Double Click MFB** (Default: Next Track)
    - **Triple Click MFB** (Default: Previous Track)
    - **Hold MFB for 2s** (Default: Fast Forward 5s)
    - **Hold MFB for 5s** (Default: Key Lock / HOLD toggle)
  - Configurable actions include Play/Pause, Next Track, Previous Track, Volume Up, Volume Down, Fast Forward, Rewind, Key Lock, Mute, or None.
- ⚡ **Aggressive Battery Optimizations:**
  - **Zero Quiescent Idle Current:** Analog headphone amplifier and MT6323 audio clocks power down when audio playback stops/pauses.
  - **Screen-Off CPU Throttling:** Automatically drops CPU clock to 598 MHz power-saving mode while screen is off, restoring dynamic clocking upon wake.
  - **Single-Core Audio Processing:** Power-gates secondary Cortex-A7 core during playback.
  - **SPM Regular Idle:** Regular idle enabled for high power efficiency without audio bus starvation.
  - **Storage Power Management:** Linux VM dirty writeback interval extended to 120s with runtime PM enabled across MMC controllers.
- 🔌 **Pause on Headphone Unplug:**
  - Full native Rockbox setting support (`Settings -> Playback Settings -> Pause on Headphone Unplug`).
  - Automatically pauses audio playback when 3.5mm headphones are removed and resumes when reconnected via the MT6572 `accdet` / `h2w` hardware detection switch.
- 💽 **Disk Access Indicator:**
  - Native Rockbox virtual disk activity LED support (`CONFIG_LED LED_VIRTUAL`).
  - Real-time disk activity indicator in the status bar and theme skins (WPS `%lh` token) during file reading, streaming, and database operations.
- 🔒 **Automatic HOLD Mode & Advanced Key Lock:**
  - **Auto-Hold:** Whenever the backlight turns off (via timeout or pressing the Power button), the device automatically enters HOLD mode.
  - **Advanced Key Lock:** Settings in `Settings -> General Settings -> System -> Advanced Key Lock` are applied while the screen is off (e.g. keep Volume or Play/Pause buttons active in your pocket). All other buttons are silently blocked without waking the screen.
  - **Instant Unhold:** Simply press the **Center button** to unhold and wake up the screen. The wake press is cleanly swallowed to avoid unintended clicks.
  - **Context Menu:** Long-pressing Center for ~1s opens the Context Menu smoothly when awake.
- 💾 **MicroSD & USB Storage:**
  - Exclusive physical MicroSD card discovery (`mmcblk1`), excluding Android eMMC clutter.
  - Supports FAT32, exFAT (SDXC 256GB+), NTFS, and ext4, including multi-partition cards.
  - MicroSD mounted as `/` (`PIVOT_ROOT`) for full `.rock` plugin and game compatibility.
  - Built-in USB Mass Storage gadget allows drag-and-drop music and theme transfer directly from Windows, Mac, or Linux.
- 🖼️ **Hardware Status Screens:**
  - `boot.png` hardware bootscreen on startup cleanly disappears upon Rockbox initialization via immediate display controller update.
  - `sdcard.png` alert if MicroSD card or `.rockbox` directory is missing (with USB Mass Storage active to add files).
  - Native Rockbox USB screen during file transfer.
  - Automatic continuation once issues are resolved.

---

## Release Assets

| File | Size | Description |
| :--- | :--- | :--- |
| `boot_rockbox.img` | 6,096,896 bytes | Flashable boot image (kernel + initramfs) for the `boot` (`BOOTIMG`) partition. Fits within 6MB partition limit with 194,560 bytes headroom. |
| `rockbox_sdcard.zip` | ~14.4 MB | SD card root files (`.rockbox` directory with all audio codecs, plugins, fonts, themes, and screen assets). |
| `SHA256SUMS.txt` | Text | SHA-256 verification checksums for all release binaries. |

### SHA-256 Checksums
```text
5d6440c8cededcc9424731bad6aab1d4335ffca27f0b5c21420883fd9943c853  boot_rockbox.img
e76a64f8e6d8146ef4da8c1c95f58e57a5ec3f361d4e2f8135997029e197cb93  rockbox_sdcard.zip
```

---

## Quick Installation Guide

### Step 1: Flash the Boot Image
1. Power off your Innioasis Y1 completely.
2. Flash `boot_rockbox.img` to the `boot` partition using either:
   - **Innioasis Updater CE:** Select *Nugget for Y1* and follow on-screen instructions.
   - **SP Flash Tool:** Load your MT6572 scatter file, check only `boot` (or `BOOTIMG`), select `boot_rockbox.img`, click **Download**, and connect the powered-off device while holding Volume Down.
   - **Fastboot / python-mtk / mtkclient:**
     ```bash
     python mtk.py w boot boot_rockbox.img
     ```

### Step 2: Prepare Your MicroSD Card
1. Format your MicroSD card as **FAT32** or **exFAT**.
2. Extract `rockbox_sdcard.zip` directly to the root of the MicroSD card so that `.rockbox` is located at:
   ```text
   (MicroSD Root)/
   ├── .rockbox/
   │   ├── codecs/
   │   ├── rocks/
   │   ├── fonts/
   │   ├── themes/
   │   └── ...
   └── Music/
   ```
3. Insert the card into your Innioasis Y1.

### Step 3: Power On & Enjoy
1. Power on your Innioasis Y1. The Nugget bootscreen will appear and load into Rockbox in ~8 seconds.
2. Connect USB to your PC anytime to manage your music and themes via USB Mass Storage.

---

## Keypad & Button Reference

| Action | Control | Notes |
| :--- | :--- | :--- |
| **Scroll / Navigate** | Click Wheel | Clockwise = Next/Down, Counter-Clockwise = Prev/Up |
| **Select / OK** | Center Button (Short Press) | Activates selected menu item / opens folder |
| **Context Menu** | Center Button (Hold ~1s) | Opens track/file context menu |
| **Auto HOLD Mode (Screen Off)** | Backlight Timeout / Power | Automatically engages HOLD mode with Advanced Key Lock settings applied |
| **UNHOLD Mode (Unlock)** | Center Button | Unlocks keypad and turns screen ON (wake click is swallowed) |
| **Back / Menu** | Top Menu Button (Short Press) | Navigates back / returns to parent menu |
| **Quick Screen** | Top Menu Button (Long Press) | Opens Rockbox Quick Screen settings |
| **Play / Pause** | Bottom Play Button (Short Press) | Toggles playback |
| **Stop Playback** | Bottom Play Button (Long Press) | Stops audio playback |
| **Previous / Skip Back** | Left Button | Skip to previous track / seek back |
| **Next / Skip Forward** | Right Button | Skip to next track / seek forward |
| **Volume Up / Down** | Side Volume Buttons | 1 dB steps (-74 dB to +6 dB) |
| **Power Off / Sleep** | Power Button | Native power management / auto-sleep timers |

