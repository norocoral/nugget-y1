#!/usr/bin/env python3
"""
setup_rootfs.py - Populates the Innioasis Y1 initramfs filesystem skeleton
"""

import os
import shutil

ROOTFS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "rootfs"))

DIRS = [
    "bin", "sbin", "usr/bin", "usr/sbin", "lib",
    "dev", "proc", "sys", "mnt/sdcard", "mnt/extsd", "mnt/usb", "storage",
    "etc", "var", "tmp"
]

def main():
    print(f"[*] Creating rootfs structure at: {ROOTFS}")
    for d in DIRS:
        p = os.path.join(ROOTFS, d)
        os.makedirs(p, exist_ok=True)

    # Copy init script
    init_src = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "initramfs", "init"))
    init_dst = os.path.join(ROOTFS, "init")
    shutil.copyfile(init_src, init_dst)
    print(f"[+] Installed /init")

    # Copy audio init script
    audio_src = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "audio", "init_audio.sh"))
    audio_dst = os.path.join(ROOTFS, "bin", "init_audio.sh")
    shutil.copyfile(audio_src, audio_dst)
    print(f"[+] Installed /bin/init_audio.sh")

    # Create etc/mdev.conf
    mdev_conf = os.path.join(ROOTFS, "etc", "mdev.conf")
    with open(mdev_conf, "w", newline="\n") as f:
        f.write("# /etc/mdev.conf\n")
        f.write("event[0-9]+ 0:0 660 =input/\n")
        f.write("mice 0:0 660 =input/\n")
        f.write("mouse[0-9]+ 0:0 660 =input/\n")
        f.write("fb[0-9]+ 0:0 660 =graphics/\n")
        f.write("eac 0:0 666\n")
    print(f"[+] Created /etc/mdev.conf")

    # Create etc/passwd and etc/group
    with open(os.path.join(ROOTFS, "etc", "passwd"), "w", newline="\n") as f:
        f.write("root:x:0:0:root:/root:/bin/sh\n")
    with open(os.path.join(ROOTFS, "etc", "group"), "w", newline="\n") as f:
        f.write("root:x:0:\n")

    print("[+] Rootfs skeleton successfully prepared!")

if __name__ == "__main__":
    main()

