#!/usr/bin/env python3
"""
build.py - Cross-platform Build & Assembly Orchestrator for Innioasis Y1
Handles rootfs assembly, initramfs compression with MTK header, and boot.img generation.
"""

import os
import sys
import shutil
import subprocess
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKSPACE_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
STOCK_BOOT = os.path.join(WORKSPACE_ROOT, "!stock_rom", "boot.img")
UNPACKED_DIR = os.path.join(SCRIPT_DIR, "stock_unpacked")
ROOTFS_DIR = os.path.join(SCRIPT_DIR, "rootfs")
SDCARD_ZIP = os.path.join(SCRIPT_DIR, "rockbox_sdcard.zip")
RAMDISK_OUT = os.path.join(SCRIPT_DIR, "ramdisk_rockbox.img")
BOOT_OUT = os.path.join(SCRIPT_DIR, "boot_rockbox.img")

TOOLS_DIR = os.path.join(SCRIPT_DIR, "tools")
MKBOOTIMG = os.path.join(TOOLS_DIR, "mkbootimg_mtk.py")
BUILD_INITRAMFS = os.path.join(TOOLS_DIR, "build_initramfs.py")
SETUP_ROOTFS = os.path.join(TOOLS_DIR, "setup_rootfs.py")
INIT_SOURCE = os.path.join(SCRIPT_DIR, "initramfs", "init.c")
CROSS_COMPILE = os.environ.get("CROSS_COMPILE", "arm-linux-gnueabihf-")
CC = CROSS_COMPILE + "gcc"
STRIP = CROSS_COMPILE + "strip"
ARM_LIB_ROOT = os.environ.get("ARM_LIB_ROOT", "/usr/arm-linux-gnueabihf/lib")


def copy_runtime_libraries():
    os.makedirs(os.path.join(ROOTFS_DIR, "lib"), exist_ok=True)
    for name in ("ld-linux-armhf.so.3", "libc.so.6"):
        target = os.path.join(ROOTFS_DIR, "lib", name)
        if not os.path.isfile(target):
            for current_root, _, files in os.walk(ARM_LIB_ROOT):
                if name in files:
                    shutil.copy2(os.path.join(current_root, name), target)
                    break
        if not os.path.isfile(target):
            raise SystemExit(f"[!] Missing {name} under {ARM_LIB_ROOT}; install the ARM hard-float sysroot.")
        subprocess.call([STRIP, "--strip-unneeded", target], stderr=subprocess.DEVNULL)


def main():
    print("==================================================")
    print(" Innioasis Y1 Hosted Linux Port - Build & Assembly")
    print("==================================================")

    # 1. Unpack stock boot.img to get stock kernel if not present
    if not os.path.exists(os.path.join(UNPACKED_DIR, "kernel_mtk.bin")):
        print("[1/4] Unpacking stock boot.img to extract vendor kernel...")
        subprocess.check_call([sys.executable, MKBOOTIMG, "unpack", STOCK_BOOT, "-o", UNPACKED_DIR])
    else:
        print("[1/4] Stock kernel already extracted in stock_unpacked/")

    # 2. Setup rootfs structure
    print("[2/4] Setting up root filesystem skeleton...")
    subprocess.check_call([sys.executable, SETUP_ROOTFS])

    # 3. Verify the Rockbox executable is packaged for the MicroSD card.
    if not os.path.isfile(SDCARD_ZIP):
        raise SystemExit("[!] Missing rockbox_sdcard.zip. Run build.sh to build the SD-card system files first.")
    try:
        with zipfile.ZipFile(SDCARD_ZIP) as archive:
            if ".rockbox/rockbox" not in archive.namelist():
                raise SystemExit("[!] rockbox_sdcard.zip must contain .rockbox/rockbox. Rebuild it with build.sh.")
    except zipfile.BadZipFile as exc:
        raise SystemExit(f"[!] Invalid SD-card archive: {exc}")

    # 4. Build the PID 1 executable for the initramfs; Rockbox itself stays on SD.
    print(f"[+] Compiling initramfs PID 1 with {CC}...")
    os.makedirs(ROOTFS_DIR, exist_ok=True)
    copy_runtime_libraries()
    subprocess.check_call([CC, "-O2", "-Wall", INIT_SOURCE, "-o", os.path.join(ROOTFS_DIR, "init")])
    subprocess.check_call([STRIP, os.path.join(ROOTFS_DIR, "init")])
    for stale_path in (os.path.join(ROOTFS_DIR, "rockbox"), os.path.join(ROOTFS_DIR, ".rockbox")):
        if os.path.isdir(stale_path):
            shutil.rmtree(stale_path)
        elif os.path.exists(stale_path):
            os.remove(stale_path)

    # 5. Build initramfs
    print("[3/4] Packaging initramfs (CPIO gzip + MTK ROOTFS header)...")
    subprocess.check_call([sys.executable, BUILD_INITRAMFS, ROOTFS_DIR, "-o", RAMDISK_OUT])

    # 6. Pack boot.img
    print("[4/4] Assembling flashable boot_rockbox.img...")
    kernel_path = os.path.join(UNPACKED_DIR, "kernel_mtk.bin")
    subprocess.check_call([
        sys.executable, MKBOOTIMG, "pack",
        "-k", kernel_path,
        "-r", RAMDISK_OUT,
        "-o", BOOT_OUT
    ])

    print("\n==================================================")
    print(" ASSEMBLY COMPLETE!")
    print(f" Flashable boot image: {BOOT_OUT}")
    print(f" SD-card system files: {SDCARD_ZIP} (extract to the physical MicroSD root)")
    print(" Flash via fastboot:   fastboot flash boot boot_rockbox.img")
    print(" Or load into SP Flash Tool replacing boot.img")
    print("==================================================")


if __name__ == "__main__":
    main()

