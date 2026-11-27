# Nugget Y1 QEMU Emulator (`configapp` & `meui`)

Runs the real `arm-linux-gnueabihf` binaries (`configapp` and `meui`) under `qemu-arm` with virtualized `/dev/graphics/fb0` (`480x360@32bpp`), `/dev/input/event0`, battery/backlight sysfs, and `/mnt/sdcard` mapped to `dist/rockbox_sdcard`.

## Prerequisites (WSL)
```bash
sudo apt update && sudo apt install -y qemu-user
```

## Usage
```bash
./tools/qemu_emu/run_emu.sh configapp
# or
./tools/qemu_emu/run_emu.sh meui
# or Rockbox (with optional SD card directory, e.g. /mnt/e):
./tools/qemu_emu/run_emu.sh rockbox
./tools/qemu_emu/run_emu.sh rockbox /mnt/e
```
Then open `http://127.0.0.1:8765` in your browser.

## Controls
- **Arrow Up / Down** or **Mouse Wheel**: Clickwheel Scroll
- **Enter**: Select
- **Esc / Backspace**: Menu / Back
- **Space**: Play / Pause
- **Arrow Left / Right**: Previous / Next
