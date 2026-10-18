#!/usr/bin/env python3
"""
build_initramfs.py - Initramfs Archive Packer for MediaTek MT6572
Packages a directory into a standard newc CPIO archive, compresses with gzip,
and prepends the 512-byte MediaTek ROOTFS header.

Author: Embedded Linux Systems Engineer for Innioasis Y1 Port
"""

import os
import sys
import gzip
import struct
import argparse
from io import BytesIO

MTK_MAGIC = 0x58881688


def create_mtk_header(payload_size: int, payload_type: str = "ROOTFS") -> bytes:
    type_bytes = payload_type.encode('ascii').ljust(32, b'\x00')
    header = struct.pack('<II', MTK_MAGIC, payload_size) + type_bytes
    header += b'\xff' * (512 - len(header))
    return header


def cpio_entry(name: str, content: bytes, mode: int = 0o100755, mtime: int = 0) -> bytes:
    """Creates a newc CPIO archive header and entry."""
    # cpio newc header: 110 bytes
    # magic 6, ino 8, mode 8, uid 8, gid 8, nlink 8, mtime 8, filesize 8,
    # maj 8, min 8, rmaj 8, rmin 8, namesize 8, chksum 8
    name_bytes = name.encode('ascii') + b'\x00'
    namesize = len(name_bytes)
    filesize = len(content)

    hdr = f"070701{0:08x}{mode:08x}{0:08x}{0:08x}{1:08x}{mtime:08x}{filesize:08x}{0:08x}{0:08x}{0:08x}{0:08x}{namesize:08x}{0:08x}".encode('ascii')

    # Pad name to 4 bytes boundary relative to entry start (110 bytes header + namesize)
    pad_name = (4 - ((110 + namesize) % 4)) % 4
    pad_file = (4 - (filesize % 4)) % 4

    return hdr + name_bytes + (b'\x00' * pad_name) + content + (b'\x00' * pad_file)


def build_cpio(root_dir: str) -> bytes:
    """Recursively packs root_dir into CPIO format."""
    buf = bytearray()

    # Sort files for deterministic output
    for root, dirs, files in os.walk(root_dir):
        # Add directories (handle symlinks to directories)
        dirs_to_process = []
        for d in sorted(dirs):
            full_path = os.path.join(root, d)
            rel_path = os.path.relpath(full_path, root_dir).replace('\\', '/')
            if os.path.islink(full_path):
                target = os.readlink(full_path).encode('utf-8')
                buf.extend(cpio_entry(rel_path, target, mode=0o120777))
            else:
                dirs_to_process.append(d)
                buf.extend(cpio_entry(rel_path, b'', mode=0o040755))
        dirs[:] = dirs_to_process

        # Add files
        for f in sorted(files):
            full_path = os.path.join(root, f)
            rel_path = os.path.relpath(full_path, root_dir).replace('\\', '/')

            if os.path.islink(full_path):
                target = os.readlink(full_path).encode('utf-8')
                buf.extend(cpio_entry(rel_path, target, mode=0o120777))
            else:
                is_exec = os.access(full_path, os.X_OK) or rel_path in ["init", "init_audio.sh", "rockbox"] or rel_path.startswith("bin/") or rel_path.startswith("sbin/")
                mode = 0o100755 if is_exec else 0o100644

                with open(full_path, 'rb') as fp:
                    data = fp.read()
                buf.extend(cpio_entry(rel_path, data, mode=mode))

    # CPIO TRAILER
    buf.extend(cpio_entry("TRAILER!!!", b'', mode=0))
    # Pad to 512 bytes
    pad = (512 - (len(buf) % 512)) % 512
    buf.extend(b'\x00' * pad)
    return bytes(buf)


def pack_initramfs(source_dir: str, output_path: str, add_mtk_header: bool = True):
    print(f"[*] Packaging directory '{source_dir}' into CPIO archive...")
    cpio_data = build_cpio(source_dir)
    print(f"[*] Uncompressed CPIO size: {len(cpio_data)} bytes")

    print("[*] Compressing with gzip...")
    compressed_io = BytesIO()
    with gzip.GzipFile(fileobj=compressed_io, mode='wb', compresslevel=9, mtime=0) as gz:
        gz.write(cpio_data)
    gz_data = compressed_io.getvalue()
    print(f"[*] Compressed gzip size: {len(gz_data)} bytes")

    final_payload = gz_data
    if add_mtk_header:
        print("[*] Adding 512-byte MediaTek ROOTFS header...")
        header = create_mtk_header(len(gz_data), "ROOTFS")
        final_payload = header + gz_data

    with open(output_path, 'wb') as f:
        f.write(final_payload)
    print(f"[+] Successfully generated initramfs: {output_path} ({len(final_payload)} bytes)")


def main():
    parser = argparse.ArgumentParser(description="Innioasis Y1 Initramfs Builder")
    parser.add_argument("source_dir", help="Directory containing root filesystem files")
    parser.add_argument("-o", "--output", default="ramdisk_rockbox.img", help="Output ramdisk image path")
    parser.add_argument("--no-mtk-header", action="store_true", help="Omit 512-byte MTK header")

    args = parser.parse_args()
    pack_initramfs(args.source_dir, args.output, add_mtk_header=not args.no_mtk_header)


if __name__ == "__main__":
    main()

