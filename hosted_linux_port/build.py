#!/usr/bin/env python3
"""
build.py - Cross-platform Build & Assembly Orchestrator for Innioasis Y1
Handles rootfs assembly, initramfs compression with MTK header, and boot.img generation.
"""

import os
import sys
import shutil
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKSPACE_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
STOCK_BOOT = os.path.join(WORKSPACE_ROOT, "!stock_rom", "boot.img")
UNPACKED_DIR = os.path.join(SCRIPT_DIR, "stock_unpacked")
ROOTFS_DIR = os.path.join(SCRIPT_DIR, "rootfs")
RAMDISK_OUT = os.path.join(SCRIPT_DIR, "ramdisk_rockbox.img")
BOOT_OUT = os.path.join(SCRIPT_DIR, "boot_rockbox.img")

TOOLS_DIR = os.path.join(SCRIPT_DIR, "tools")
MKBOOTIMG = os.path.join(TOOLS_DIR, "mkbootimg_mtk.py")
BUILD_INITRAMFS = os.path.join(TOOLS_DIR, "build_initramfs.py")
SETUP_ROOTFS = os.path.join(TOOLS_DIR, "setup_rootfs.py")


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

    # 3. Check for rockbox binary and assets
    rockbox_bin = os.path.join(ROOTFS_DIR, "rockbox")
    if not os.path.exists(rockbox_bin):
        print("[!] Note: 'rootfs/rockbox' binary is not yet compiled.")
        print("    You can compile it via build.sh inside WSL / Linux using:")
        print("    arm-linux-gnueabihf-gcc toolchain with target 310.")
    else:
        print(f"[+] Found Rockbox binary: {rockbox_bin}")

    # 4. Build initramfs
    print("[3/4] Packaging initramfs (CPIO gzip + MTK ROOTFS header)...")
    subprocess.check_call([sys.executable, BUILD_INITRAMFS, ROOTFS_DIR, "-o", RAMDISK_OUT])

    # 5. Pack boot.img
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
    print(" Flash via fastboot:   fastboot flash boot boot_rockbox.img")
    print(" Or load into SP Flash Tool replacing boot.img")
    print("==================================================")


if __name__ == "__main__":
    main()

