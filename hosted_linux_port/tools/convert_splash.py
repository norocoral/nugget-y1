#!/usr/bin/env python3
"""
convert_splash.py - Convert PNG boot splash image to raw ARGB8888 framebuffer binary
Pure Python implementation using standard library zlib & struct (no external dependencies).
Outputs 32-bit ARGB8888 (little-endian BGRA / XRGB) matching MT6572 /dev/graphics/fb0.
"""

import sys
import zlib
import struct
import os

def decode_png_to_xrgb8888(png_path: str, target_w: int = 480, target_h: int = 360) -> bytes:
    with open(png_path, 'rb') as f:
        data = f.read()

    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f"Invalid PNG magic signature in {png_path}")

    idx = 8
    idat = bytearray()
    w, h, bit_depth, color_type = 0, 0, 0, 0

    while idx < len(data):
        length, chunk_type = struct.unpack('>I4s', data[idx:idx+8])
        idx += 8
        chunk_data = data[idx:idx+length]
        idx += length + 4  # skip crc

        if chunk_type == b'IHDR':
            w, h, bit_depth, color_type, comp, filter_m, interlace = struct.unpack('>IIBBBBB', chunk_data)
            if bit_depth != 8:
                raise ValueError(f"Unsupported bit depth: {bit_depth} (expected 8-bit)")
            if color_type not in (2, 6):  # 2: RGB, 6: RGBA
                raise ValueError(f"Unsupported color type: {color_type} (expected RGB or RGBA)")
            if interlace != 0:
                raise ValueError("Interlaced PNG not supported")
        elif chunk_type == b'IDAT':
            idat.extend(chunk_data)
        elif chunk_type == b'IEND':
            break

    if w != target_w or h != target_h:
        print(f"Notice: Image size is {w}x{h}, expected {target_w}x{target_h}")

    decomp = zlib.decompress(bytes(idat))
    bytes_per_pixel = 3 if color_type == 2 else 4
    row_bytes = w * bytes_per_pixel
    stride = 1 + row_bytes

    if len(decomp) < h * stride:
        raise ValueError(f"Decompressed data too short ({len(decomp)} bytes, expected {h * stride})")

    out_pixels = bytearray(w * h * 4)
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

            if filter_type == 0:    # None
                val = filt_byte
            elif filter_type == 1:  # Sub
                val = (filt_byte + a) & 0xff
            elif filter_type == 2:  # Up
                val = (filt_byte + b) & 0xff
            elif filter_type == 3:  # Average
                val = (filt_byte + ((a + b) >> 1)) & 0xff
            elif filter_type == 4:  # Paeth
                p = a + b - c
                pa = abs(p - a)
                pb = abs(p - b)
                pc = abs(p - c)
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

        # Convert to 32-bit little-endian (B, G, R, 0xFF)
        for c in range(w):
            if color_type == 2:  # RGB
                r_val = cur_row[c * 3]
                g_val = cur_row[c * 3 + 1]
                b_val = cur_row[c * 3 + 2]
            else:  # RGBA
                r_val = cur_row[c * 4]
                g_val = cur_row[c * 4 + 1]
                b_val = cur_row[c * 4 + 2]

            out_pixels[out_idx]     = b_val
            out_pixels[out_idx + 1] = g_val
            out_pixels[out_idx + 2] = r_val
            out_pixels[out_idx + 3] = 0xFF
            out_idx += 4

    return bytes(out_pixels)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.raw>")
        sys.exit(1)

    input_png = sys.argv[1]
    output_raw = sys.argv[2]

    if not os.path.exists(input_png):
        print(f"Error: Input file '{input_png}' not found!")
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(output_raw)), exist_ok=True)

    print(f"[*] Converting '{input_png}' to raw framebuffer binary '{output_raw}'...")
    raw_data = decode_png_to_xrgb8888(input_png, 480, 360)

    with open(output_raw, 'wb') as f:
        f.write(raw_data)

    print(f"[+] Successfully converted: {output_raw} ({len(raw_data)} bytes)")


if __name__ == "__main__":
    main()

