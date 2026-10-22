# Hosted Rockbox on Minimal Linux (Innioasis Y1)

This folder contains the build environment, minimal Linux initramfs, hardware audio/display support, and packaging tools for the **Innioasis Y1** Rockbox port. The boot image contains Linux startup and hardware support; the Rockbox executable and its system files live on the physical MicroSD card.

## Directory Structure
- `initramfs/`: PID 1 `/init` in C. Discovers and mounts physical MicroSD storage at boot, exposes it to a PC through USB Mass Storage, and displays framebuffer status screens using RAW images. USB OTG mounting and post-boot storage hotplug detection are not supported.
- `audio/`: standalone `unmute_eac.c` hardware audio test utility.
- `rootfs/`: Skeleton for the minimal initramfs ramdisk. It does not contain the Rockbox executable or `.rockbox` system directory.
- `stock_unpacked/`: Extracted stock kernel (`kernel_mtk.bin`).
- `tools/`: Build scripts, splash image converter (`convert_splash.py`), MTK boot image packer (`mkbootimg_mtk.py`), and CPIO ramdisk generator.
- `rockbox_sdcard.zip`: SD-card payload containing `.rockbox/rockbox`, Rockbox system files, RAW status screens, and bundled Bluetooth assets.
- `release/`: Flashable `boot_rockbox.img`, SD-card payload, SHA256 checksums, and release notes.

## Building
```bash
./build.sh
```

Run the full build in WSL or Linux with the ARM hard-float cross toolchain, Python 3, and `zip` installed. It builds the Rockbox executable, places it at `.rockbox/rockbox` inside `rockbox_sdcard.zip`, and converts source status PNGs to RAW. PNG entries remain in the SD archive; startup displays the RAW files.

From this directory, `build.sh` and `build.py` search parent directories for the workspace containing `!stock_rom`. Set `WORKSPACE_ROOT` if the workspace cannot be discovered automatically, or set `STOCK_BOOT` to use a different stock `boot.img` path.

Flash `boot_rockbox.img` to the boot partition, then extract `rockbox_sdcard.zip` to the root of the physical MicroSD card. The card must contain `.rockbox/rockbox` and the rest of `.rockbox`; internal eMMC is never used for Rockbox files. Startup waits on the RAW SD-card status screen if the card or executable is missing.

Y1 diagnostic logging is disabled by default. Enable **Settings → Nugget → Device → Diagnostic Logging** to append init, display, audio, Bluetooth, button, and suspend diagnostics to `/mnt/sdcard/y1_debug.txt`. The setting is stored in `.rockbox/config.cfg`; init reads it at boot to decide whether to write startup diagnostics.

`build.py` assembles the boot image from an existing `rockbox_sdcard.zip` and requires the same ARM cross toolchain to compile PID 1. It refuses to build if the archive does not contain `.rockbox/rockbox`.
