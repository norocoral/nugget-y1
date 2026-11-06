#!/usr/bin/env python3
"""
generate_bootl_assets.py - Generate pre-composited bootloader menu raw screens
Composites bg.png, pick1.png, pick2.png, pick3.png, pick4.png, and arrow.png into:
  - bootl_pick1.raw (Nugget selected)
  - bootl_pick2.raw (Android selected)
  - bootl_pick3.raw (Shut down selected)
  - bootl_pick4.raw (USB selected)
Also converts usb.png to:
  - bootl_usb.raw
Supports transparent PNGs with alpha channel blending.
Pure Python implementation using standard library zlib & struct (no PIL dependency).
Output matches 480x360 ARGB8888 (little-endian BGRA) for MT6572 /dev/graphics/fb0.
"""

import sys
import os
import zlib
import struct

def decode_png(png_path):
    with open(png_path, "rb") as f:
        data = f.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Invalid PNG magic in {png_path}")

    idx = 8
    idat = bytearray()
    w, h, bit_depth, color_type = 0, 0, 0, 0

    while idx < len(data):
        length, chunk_type = struct.unpack(">I4s", data[idx:idx+8])
        idx += 8
        chunk_data = data[idx:idx+length]
        idx += length + 4  # skip crc

        if chunk_type == b"IHDR":
            w, h, bit_depth, color_type, comp, filter_m, interlace = struct.unpack(">IIBBBBB", chunk_data)
            if bit_depth != 8:
                raise ValueError(f"Unsupported bit depth: {bit_depth}")
            if color_type not in (2, 6):
                raise ValueError(f"Unsupported color type: {color_type}")
            if interlace != 0:
                raise ValueError("Interlaced PNG not supported")
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    decomp = zlib.decompress(bytes(idat))
    bytes_per_pixel = 3 if color_type == 2 else 4
    row_bytes = w * bytes_per_pixel
    stride = 1 + row_bytes

    pixels = bytearray(w * h * 4)  # Store as RGBA in memory
    prev_row = bytearray(row_bytes)
    out_idx = 0

    for r in range(h):
        row_data = decomp[r * stride : (r + 1) * stride]
        filter_type = row_data[0]
        cur_row = bytearray(row_bytes)

        for i in range(row_bytes):
            filt_byte = row_data[1 + i]
            a = cur_row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
            b = prev_row[i]
            c = prev_row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0

            if filter_type == 0:
                val = filt_byte
            elif filter_type == 1:
                val = (filt_byte + a) & 0xff
            elif filter_type == 2:
                val = (filt_byte + b) & 0xff
            elif filter_type == 3:
                val = (filt_byte + ((a + b) >> 1)) & 0xff
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                val = (filt_byte + pr) & 0xff
            else:
                val = filt_byte
            cur_row[i] = val

        prev_row = cur_row

        for c in range(w):
            if color_type == 2:
                r_val = cur_row[c * 3]
                g_val = cur_row[c * 3 + 1]
                b_val = cur_row[c * 3 + 2]
                a_val = 255
            else:
                r_val = cur_row[c * 4]
                g_val = cur_row[c * 4 + 1]
                b_val = cur_row[c * 4 + 2]
                a_val = cur_row[c * 4 + 3]

            pixels[out_idx]     = r_val
            pixels[out_idx + 1] = g_val
            pixels[out_idx + 2] = b_val
            pixels[out_idx + 3] = a_val
            out_idx += 4

    return w, h, pixels


def overlay_sprite(dest, dest_w, dest_h, src, src_w, src_h, dest_x, dest_y):
    """Overlay src (RGBA) onto dest (RGBA) with alpha blending."""
    for sy in range(src_h):
        dy = dest_y + sy
        if dy < 0 or dy >= dest_h:
            continue
        for sx in range(src_w):
            dx = dest_x + sx
            if dx < 0 or dx >= dest_w:
                continue

            s_idx = (sy * src_w + sx) * 4
            sr = src[s_idx]
            sg = src[s_idx + 1]
            sb = src[s_idx + 2]
            sa = src[s_idx + 3]

            if sa == 0:
                continue
            # Also skip legacy pure magenta (#FF00FF)
            if sa == 255 and sr >= 250 and sg <= 5 and sb >= 250:
                continue

            d_idx = (dy * dest_w + dx) * 4
            if sa == 255:
                dest[d_idx]     = sr
                dest[d_idx + 1] = sg
                dest[d_idx + 2] = sb
                dest[d_idx + 3] = 255
            else:
                alpha = sa / 255.0
                inv_a = 1.0 - alpha
                dest[d_idx]     = int(sr * alpha + dest[d_idx]     * inv_a)
                dest[d_idx + 1] = int(sg * alpha + dest[d_idx + 1] * inv_a)
                dest[d_idx + 2] = int(sb * alpha + dest[d_idx + 2] * inv_a)
                dest[d_idx + 3] = 255


def rgba_to_fb_raw(rgba_pixels, w, h):
    """Convert RGBA to 32-bit little-endian BGRA (XRGB8888) for /dev/fb0."""
    raw = bytearray(w * h * 4)
    idx = 0
    for i in range(0, w * h * 4, 4):
        r = rgba_pixels[i]
        g = rgba_pixels[i + 1]
        b = rgba_pixels[i + 2]
        raw[idx]     = b
        raw[idx + 1] = g
        raw[idx + 2] = r
        raw[idx + 3] = 0xFF
        idx += 4
    return bytes(raw)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <assets_dir> <output_dir>")
        sys.exit(1)

    assets_dir = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    bg_path = os.path.join(assets_dir, 'bg.png')
    p1_path = os.path.join(assets_dir, 'pick1.png')
    p2_path = os.path.join(assets_dir, 'pick2.png')
    p3_path = os.path.join(assets_dir, 'pick3.png')
    p4_path = os.path.join(assets_dir, 'pick4.png')
    arr_path = os.path.join(assets_dir, 'arrow.png')
    usb_path = os.path.join(assets_dir, 'usb.png')

    for p in (bg_path, p1_path, p2_path, p3_path, p4_path, arr_path, usb_path):
        if not os.path.exists(p):
            print(f"Error: Asset '{p}' not found!")
            sys.exit(1)

    bg_w, bg_h, bg_px = decode_png(bg_path)
    p1_w, p1_h, p1_px = decode_png(p1_path)
    p2_w, p2_h, p2_px = decode_png(p2_path)
    p3_w, p3_h, p3_px = decode_png(p3_path)
    p4_w, p4_h, p4_px = decode_png(p4_path)
    arr_w, arr_h, arr_px = decode_png(arr_path)
    usb_w, usb_h, usb_px = decode_png(usb_path)

    # 3 visible slots across 480px width:
    # Slot 0: x = 6, y = 67
    # Slot 1: x = 164, y = 67
    # Slot 2: x = 322, y = 67
    # Arrow (64x64) at y = 229:
    # Under Slot 0: x = 50
    # Under Slot 1: x = 208
    # Under Slot 2: x = 366
    pages = [
        # (filename, [slot0_sprite, slot1_sprite, slot2_sprite], arrow_x, arrow_y)
        ("bootl_pick1.raw", [(p1_px, p1_w, p1_h), (p2_px, p2_w, p2_h), (p3_px, p3_w, p3_h)], 50, 229),
        ("bootl_pick2.raw", [(p1_px, p1_w, p1_h), (p2_px, p2_w, p2_h), (p3_px, p3_w, p3_h)], 208, 229),
        ("bootl_pick3.raw", [(p2_px, p2_w, p2_h), (p3_px, p3_w, p3_h), (p4_px, p4_w, p4_h)], 208, 229),
        ("bootl_pick4.raw", [(p2_px, p2_w, p2_h), (p3_px, p3_w, p3_h), (p4_px, p4_w, p4_h)], 366, 229),
    ]

    slot_x = [6, 164, 322]
    slot_y = 67

    for filename, slots, ax, ay in pages:
        screen_px = bytearray(bg_px)
        for i, (spr_px, spr_w, spr_h) in enumerate(slots):
            overlay_sprite(screen_px, bg_w, bg_h, spr_px, spr_w, spr_h, slot_x[i], slot_y)
        overlay_sprite(screen_px, bg_w, bg_h, arr_px, arr_w, arr_h, ax, ay)

        raw_data = rgba_to_fb_raw(screen_px, bg_w, bg_h)
        out_path = os.path.join(out_dir, filename)
        with open(out_path, "wb") as f:
            f.write(raw_data)
        print(f"[+] Successfully generated: {out_path} ({len(raw_data)} bytes)")

    # Generate bootl_usb.raw directly from usb.png
    usb_raw = rgba_to_fb_raw(usb_px, usb_w, usb_h)
    usb_out = os.path.join(out_dir, "bootl_usb.raw")
    with open(usb_out, "wb") as f:
        f.write(usb_raw)
    print(f"[+] Successfully generated: {usb_out} ({len(usb_raw)} bytes)")

if __name__ == "__main__":
    main()
