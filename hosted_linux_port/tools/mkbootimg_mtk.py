#!/usr/bin/env python3
"""
mkbootimg_mtk.py - MediaTek Android Boot Image Packer & Unpacker
Supports MTK 512-byte header encapsulation (Magic: 0x58881688)

Author: Embedded Linux Systems Engineer for Innioasis Y1 Port
"""

import os
import sys
import struct
import argparse

MTK_MAGIC = 0x58881688
BOOT_MAGIC = b"ANDROID!"
DEFAULT_PAGE_SIZE = 2048

# Default MediaTek MT6572 Load Addresses
DEFAULT_BASE = 0x10000000
DEFAULT_KERNEL_OFFSET = 0x00008000
DEFAULT_RAMDISK_OFFSET = 0x01000000
DEFAULT_TAGS_OFFSET = 0x00000100


def create_mtk_header(payload_size: int, payload_type: str) -> bytes:
    """
    Creates a 512-byte MediaTek header.
    Format:
      magic (uint32) = 0x58881688
      size (uint32)  = payload size in bytes (excluding this 512-byte header)
      type (char[32])= 'KERNEL' or 'ROOTFS' (null padded)
      padding        = 0xff to 512 bytes
    """
    type_bytes = payload_type.encode('ascii').ljust(32, b'\x00')
    header = struct.pack('<II', MTK_MAGIC, payload_size) + type_bytes
    header += b'\xff' * (512 - len(header))
    return header


def has_mtk_header(data: bytes) -> bool:
    if len(data) < 512:
        return False
    magic = struct.unpack('<I', data[:4])[0]
    return magic == MTK_MAGIC


def ensure_mtk_header(data: bytes, payload_type: str) -> bytes:
    """Ensures data has an MTK header with the correct payload type and size."""
    if has_mtk_header(data):
        return data
    print(f"[*] Prepending 512-byte MTK header ({payload_type}, size={len(data)} bytes)...")
    return create_mtk_header(len(data), payload_type) + data


def pad_to_pagesize(data: bytes, page_size: int) -> bytes:
    remainder = len(data) % page_size
    if remainder != 0:
        data += b'\x00' * (page_size - remainder)
    return data


def pack_bootimg(kernel_path: str, ramdisk_path: str, output_path: str,
                 base: int = DEFAULT_BASE,
                 kernel_offset: int = DEFAULT_KERNEL_OFFSET,
                 ramdisk_offset: int = DEFAULT_RAMDISK_OFFSET,
                 tags_offset: int = DEFAULT_TAGS_OFFSET,
                 page_size: int = DEFAULT_PAGE_SIZE,
                 cmdline: str = ""):
    print(f"[*] Reading kernel from {kernel_path}...")
    with open(kernel_path, 'rb') as f:
        kernel_data = f.read()

    print(f"[*] Reading ramdisk from {ramdisk_path}...")
    with open(ramdisk_path, 'rb') as f:
        ramdisk_data = f.read()

    # Prepend MTK headers if not present
    kernel_data = ensure_mtk_header(kernel_data, "KERNEL")
    ramdisk_data = ensure_mtk_header(ramdisk_data, "ROOTFS")

    k_size = len(kernel_data)
    r_size = len(ramdisk_data)
    k_addr = base + kernel_offset
    r_addr = base + ramdisk_offset
    tags_addr = base + tags_offset

    print(f"[*] Assembling Android boot.img:")
    print(f"    Kernel size:  {k_size} bytes (Load Addr: {hex(k_addr)})")
    print(f"    Ramdisk size: {r_size} bytes (Load Addr: {hex(r_addr)})")
    print(f"    Tags Addr:    {hex(tags_addr)}")
    print(f"    Page Size:    {page_size}")

    # Build standard Android boot header (page 0)
    # struct boot_img_hdr {
    #   char magic[8];
    #   uint32 kernel_size;
    #   uint32 kernel_addr;
    #   uint32 ramdisk_size;
    #   uint32 ramdisk_addr;
    #   uint32 second_size;
    #   uint32 second_addr;
    #   uint32 tags_addr;
    #   uint32 page_size;
    #   uint32 unused[2];
    #   char name[16];
    #   char cmdline[512];
    #   uint32 id[8];
    #   char extra_cmdline[1024];
    # }
    hdr = bytearray(page_size)
    struct.pack_into('<8s8I', hdr, 0,
                     BOOT_MAGIC,
                     k_size, k_addr,
                     r_size, r_addr,
                     0, 0,  # second_size, second_addr
                     tags_addr,
                     page_size)

    # Copy cmdline
    cmdline_bytes = cmdline.encode('ascii')[:512]
    hdr[64:64+len(cmdline_bytes)] = cmdline_bytes

    # Pad sections to page boundaries
    kernel_padded = pad_to_pagesize(kernel_data, page_size)
    ramdisk_padded = pad_to_pagesize(ramdisk_data, page_size)

    with open(output_path, 'wb') as f:
        f.write(hdr)
        f.write(kernel_padded)
        f.write(ramdisk_padded)

    total_size = page_size + len(kernel_padded) + len(ramdisk_padded)
    print(f"[+] Successfully generated flashable boot image: {output_path} ({total_size} bytes)")


def unpack_bootimg(bootimg_path: str, out_dir: str):
    os.makedirs(out_dir, exist_ok=True)
    with open(bootimg_path, 'rb') as f:
        hdr = f.read(2048)
        magic = hdr[:8]
        if magic != BOOT_MAGIC:
            raise ValueError(f"Invalid boot image magic: {magic}")

        k_size, k_addr, r_size, r_addr, s_size, s_addr, tags_addr, page_size = struct.unpack('<8I', hdr[8:40])
        cmdline = hdr[64:576].split(b'\x00')[0].decode('latin1')

        print(f"[+] Boot image details:")
        print(f"    Page size:    {page_size}")
        print(f"    Kernel size:  {k_size} (Addr: {hex(k_addr)})")
        print(f"    Ramdisk size: {r_size} (Addr: {hex(r_addr)})")
        print(f"    Tags Addr:    {hex(tags_addr)}")
        print(f"    Cmdline:      '{cmdline}'")

        # Read kernel
        f.seek(page_size)
        kernel_data = f.read(k_size)
        kernel_out = os.path.join(out_dir, "kernel_mtk.bin")
        with open(kernel_out, 'wb') as kf:
            kf.write(kernel_data)
        print(f"[+] Extracted MTK kernel: {kernel_out}")

        # If kernel has MTK header, extract raw zImage too
        if has_mtk_header(kernel_data):
            zimage_out = os.path.join(out_dir, "zImage")
            with open(zimage_out, 'wb') as zf:
                zf.write(kernel_data[512:])
            print(f"[+] Extracted raw kernel zImage: {zimage_out}")

        # Read ramdisk
        k_pages = (k_size + page_size - 1) // page_size
        rd_offset = page_size + k_pages * page_size
        f.seek(rd_offset)
        ramdisk_data = f.read(r_size)
        ramdisk_out = os.path.join(out_dir, "ramdisk_mtk.img")
        with open(ramdisk_out, 'wb') as rf:
            rf.write(ramdisk_data)
        print(f"[+] Extracted MTK ramdisk: {ramdisk_out}")

        # If ramdisk has MTK header, extract raw cpio.gz too
        if has_mtk_header(ramdisk_data):
            raw_rd_out = os.path.join(out_dir, "ramdisk.cpio.gz")
            with open(raw_rd_out, 'wb') as rawf:
                rawf.write(ramdisk_data[512:])
            print(f"[+] Extracted raw ramdisk archive: {raw_rd_out}")


def main():
    parser = argparse.ArgumentParser(description="MediaTek Android Boot Image Tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # Unpack command
    unpack_p = subparsers.add_parser("unpack", help="Unpack a boot image")
    unpack_p.add_argument("bootimg", help="Path to input boot.img")
    unpack_p.add_argument("-o", "--output-dir", default="unpacked_boot", help="Output directory")

    # Pack command
    pack_p = subparsers.add_parser("pack", help="Pack a boot image")
    pack_p.add_argument("-k", "--kernel", required=True, help="Path to kernel (zImage or with MTK header)")
    pack_p.add_argument("-r", "--ramdisk", required=True, help="Path to ramdisk (cpio.gz or with MTK header)")
    pack_p.add_argument("-o", "--output", default="boot_new.img", help="Output boot.img path")
    pack_p.add_argument("--base", type=lambda x: int(x, 0), default=DEFAULT_BASE)
    pack_p.add_argument("--kernel-offset", type=lambda x: int(x, 0), default=DEFAULT_KERNEL_OFFSET)
    pack_p.add_argument("--ramdisk-offset", type=lambda x: int(x, 0), default=DEFAULT_RAMDISK_OFFSET)
    pack_p.add_argument("--tags-offset", type=lambda x: int(x, 0), default=DEFAULT_TAGS_OFFSET)
    pack_p.add_argument("--pagesize", type=int, default=DEFAULT_PAGE_SIZE)
    pack_p.add_argument("--cmdline", default="")

    args = parser.parse_args()
    if args.command == "unpack":
        unpack_bootimg(args.bootimg, args.output_dir)
    elif args.command == "pack":
        pack_bootimg(args.kernel, args.ramdisk, args.output,
                     base=args.base,
                     kernel_offset=args.kernel_offset,
                     ramdisk_offset=args.ramdisk_offset,
                     tags_offset=args.tags_offset,
                     page_size=args.pagesize,
                     cmdline=args.cmdline)


if __name__ == "__main__":
    main()

