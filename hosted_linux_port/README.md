# Hosted Rockbox on Minimal Linux (Innioasis Y1)

This folder contains the complete build environment, PID 1 initramfs, hardware audio/display drivers, and packaging tools for the **Innioasis Y1** Hosted Rockbox Linux port.

## Directory Structure
- `initramfs/`: Production PID 1 `/init` in C. Handles early boot, storage discovery, USB Mass Storage gadget, hardware unmute, and graphical framebuffer splash screens.
- `audio/`: `unmute_eac.c` hardware audio initialization and PMIC register control.
- `rootfs/`: Skeleton for the minimal initramfs ramdisk.
- `stock_unpacked/`: Extracted stock kernel (`kernel_mtk.bin`).
- `tools/`: Build scripts, splash image converter (`convert_splash.py`), MTK boot image packer (`mkbootimg_mtk.py`), and CPIO ramdisk generator.
- `release/`: Flashable `boot_rockbox.img`, `rockbox_sdcard.zip`, SHA256 checksums, and release notes.

## Building
```bash
./build.sh
```

