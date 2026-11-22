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
import subprocess
import tempfile

_FONT_5X7 = {
    ' ': (0x00, 0x00, 0x00, 0x00, 0x00),
    '.': (0x00, 0x60, 0x60, 0x00, 0x00),
    '-': (0x08, 0x08, 0x08, 0x08, 0x08),
    '_': (0x40, 0x40, 0x40, 0x40, 0x40),
    '/': (0x20, 0x10, 0x08, 0x04, 0x02),
    ':': (0x00, 0x36, 0x36, 0x00, 0x00),
    '0': (0x3E, 0x51, 0x49, 0x45, 0x3E),
    '1': (0x00, 0x42, 0x7F, 0x40, 0x00),
    '2': (0x42, 0x61, 0x51, 0x49, 0x46),
    '3': (0x21, 0x41, 0x45, 0x4B, 0x31),
    '4': (0x18, 0x14, 0x12, 0x7F, 0x10),
    '5': (0x27, 0x45, 0x45, 0x45, 0x39),
    '6': (0x3C, 0x4A, 0x49, 0x49, 0x30),
    '7': (0x01, 0x71, 0x09, 0x05, 0x03),
    '8': (0x36, 0x49, 0x49, 0x49, 0x36),
    '9': (0x06, 0x49, 0x49, 0x29, 0x1E),
    'A': (0x7E, 0x11, 0x11, 0x11, 0x7E),
    'B': (0x7F, 0x49, 0x49, 0x49, 0x36),
    'C': (0x3E, 0x41, 0x41, 0x41, 0x22),
    'D': (0x7F, 0x41, 0x41, 0x22, 0x1C),
    'E': (0x7F, 0x49, 0x49, 0x49, 0x41),
    'F': (0x7F, 0x09, 0x09, 0x09, 0x01),
    'G': (0x3E, 0x41, 0x49, 0x49, 0x7A),
    'H': (0x7F, 0x08, 0x08, 0x08, 0x7F),
    'I': (0x00, 0x41, 0x7F, 0x41, 0x00),
    'J': (0x20, 0x40, 0x41, 0x3F, 0x01),
    'K': (0x7F, 0x08, 0x14, 0x22, 0x41),
    'L': (0x7F, 0x40, 0x40, 0x40, 0x40),
    'M': (0x7F, 0x02, 0x0C, 0x02, 0x7F),
    'N': (0x7F, 0x04, 0x08, 0x10, 0x7F),
    'O': (0x3E, 0x41, 0x41, 0x41, 0x3E),
    'P': (0x7F, 0x09, 0x09, 0x09, 0x06),
    'Q': (0x3E, 0x41, 0x51, 0x21, 0x5E),
    'R': (0x7F, 0x09, 0x19, 0x29, 0x46),
    'S': (0x46, 0x49, 0x49, 0x49, 0x31),
    'T': (0x01, 0x01, 0x7F, 0x01, 0x01),
    'U': (0x3F, 0x40, 0x40, 0x40, 0x3F),
    'V': (0x1F, 0x20, 0x40, 0x20, 0x1F),
    'W': (0x3F, 0x40, 0x38, 0x40, 0x3F),
    'X': (0x63, 0x14, 0x08, 0x14, 0x63),
    'Y': (0x07, 0x08, 0x70, 0x08, 0x07),
    'Z': (0x61, 0x51, 0x49, 0x45, 0x43),
}

def _ensure_ttf_renderer() -> tuple[str | None, str | None]:
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    main_dir = os.path.dirname(tools_dir)
    font_candidates = [
        os.path.join(main_dir, "configapp", "assets", "fonts", "WantedSans-SemiBold.ttf"),
        os.path.join(main_dir, "configapp", "assets", "fonts", "WantedSans-Regular.ttf"),
    ]
    font_path = next((p for p in font_candidates if os.path.isfile(p)), None)
    if not font_path:
        return None, None

    src_c = os.path.join(tools_dir, "ttf_overlay.c")
    inc_dir = os.path.join(main_dir, "configapp", "third_party")
    bin_path = os.path.join(tempfile.gettempdir(), "y1_ttf_overlay")
    if not os.path.isfile(bin_path) or os.path.getmtime(src_c) > os.path.getmtime(bin_path):
        subprocess.check_call(["gcc", "-O2", f"-I{inc_dir}", src_c, "-o", bin_path, "-lm"])
    return bin_path, font_path


def _draw_text_xrgb(pixels: bytearray, w: int, h: int, lines: list[str], scale: int = 2) -> None:
    bin_path, font_path = _ensure_ttf_renderer()
    px_size = 16 if scale >= 2 else 12
    line_gap = 3
    margin_x = 12
    margin_y = 12

    if bin_path and font_path:
        rendered = []
        for text in lines:
            out = subprocess.check_output([bin_path, font_path, str(px_size), text])
            rw, rh = struct.unpack("<II", out[:8])
            alpha_buf = out[8:8 + rw * rh]
            rendered.append((rw, rh, alpha_buf))

        total_h = sum(rh for _, rh, _ in rendered) + max(0, len(rendered) - 1) * line_gap
        y_cur = h - margin_y - total_h

        for rw, rh, alpha_buf in rendered:
            x0 = max(0, margin_x - 3)
            y0 = max(0, y_cur - 2)
            x1 = min(w, margin_x + rw + 3)
            y1 = min(h, y_cur + rh + 2)

            lum_sum = 0.0
            cnt = 0
            for py in range(y0, y1):
                row_off = py * w * 4
                for px in range(x0, x1):
                    idx = row_off + px * 4
                    b_v, g_v, r_v = pixels[idx], pixels[idx + 1], pixels[idx + 2]
                    lum_sum += 0.2126 * r_v + 0.7152 * g_v + 0.0722 * b_v
                    cnt += 1

            avg_lum = (lum_sum / cnt) if cnt > 0 else 0.0
            fg_r, fg_g, fg_b = (18, 20, 26) if avg_lum >= 140.0 else (245, 247, 250)

            for r in range(rh):
                dy = y_cur + r
                if not (0 <= dy < h):
                    continue
                for c in range(rw):
                    dx = margin_x + c
                    if not (0 <= dx < w):
                        continue
                    a = alpha_buf[r * rw + c]
                    if a == 0:
                        continue
                    idx = (dy * w + dx) * 4
                    if a == 255:
                        pixels[idx] = fg_b
                        pixels[idx + 1] = fg_g
                        pixels[idx + 2] = fg_r
                        pixels[idx + 3] = 0xFF
                    else:
                        inv = 255 - a
                        pixels[idx] = (fg_b * a + pixels[idx] * inv) // 255
                        pixels[idx + 1] = (fg_g * a + pixels[idx + 1] * inv) // 255
                        pixels[idx + 2] = (fg_r * a + pixels[idx + 2] * inv) // 255
                        pixels[idx + 3] = 0xFF
            y_cur += rh + line_gap
        return

    # Fallback 5x7 bitmap renderer (no drop shadow, adaptive contrast)
    line_h = 7 * scale
    line_gap = 4
    total_h = len(lines) * line_h + max(0, len(lines) - 1) * line_gap
    y0 = h - margin_y - total_h

    def set_px(px: int, py: int, r: int, g: int, b: int) -> None:
        if 0 <= px < w and 0 <= py < h:
            idx = (py * w + px) * 4
            pixels[idx] = b
            pixels[idx + 1] = g
            pixels[idx + 2] = r
            pixels[idx + 3] = 0xFF

    for li, text in enumerate(lines):
        y_base = y0 + li * (line_h + line_gap)
        tw = len(text) * 6 * scale
        bx0, by0 = max(0, margin_x - 2), max(0, y_base - 2)
        bx1, by1 = min(w, margin_x + tw + 2), min(h, y_base + line_h + 2)
        lum_sum = 0.0
        cnt = 0
        for py in range(by0, by1):
            for px in range(bx0, bx1):
                idx = (py * w + px) * 4
                lum_sum += 0.2126 * pixels[idx + 2] + 0.7152 * pixels[idx + 1] + 0.0722 * pixels[idx]
                cnt += 1
        avg_lum = (lum_sum / cnt) if cnt > 0 else 0.0
        fg_r, fg_g, fg_b = (18, 20, 26) if avg_lum >= 140.0 else (245, 247, 250)

        x_cur = margin_x
        for ch in text.upper():
            glyph = _FONT_5X7.get(ch, _FONT_5X7[' '])
            for col_idx, col_bits in enumerate(glyph):
                for row_idx in range(7):
                    if col_bits & (1 << row_idx):
                        for sy in range(scale):
                            for sx in range(scale):
                                set_px(x_cur + col_idx * scale + sx, y_base + row_idx * scale + sy, fg_r, fg_g, fg_b)
            x_cur += 6 * scale

def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

def encode_xrgb8888_to_png(pixels: bytes, w: int, h: int, out_png: str) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row_off = y * w * 4
        for x in range(w):
            i = row_off + x * 4
            raw.extend((pixels[i + 2], pixels[i + 1], pixels[i], 0xFF))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    png_bytes = b'\x89PNG\r\n\x1a\n' + _png_chunk(b'IHDR', ihdr) + _png_chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + _png_chunk(b'IEND', b'')
    with open(out_png, 'wb') as f:
        f.write(png_bytes)

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
    args = sys.argv[1:]
    overlays = []
    out_png = None
    positional = []
    i = 0
    while i < len(args):
        if args[i] == '--overlay' and i + 1 < len(args):
            overlays.append(args[i + 1])
            i += 2
        elif args[i] == '--out-png' and i + 1 < len(args):
            out_png = args[i + 1]
            i += 2
        else:
            positional.append(args[i])
            i += 1

    if len(positional) < 2:
        print(f"Usage: {sys.argv[0]} [--overlay <text>]... [--out-png <path>] <input.png> <output.raw>")
        sys.exit(1)

    input_png = positional[0]
    output_raw = positional[1]

    if not os.path.exists(input_png):
        print(f"Error: Input file '{input_png}' not found!")
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(output_raw)), exist_ok=True)

    print(f"[*] Converting '{input_png}' to raw framebuffer binary '{output_raw}'...")
    raw_data = bytearray(decode_png_to_xrgb8888(input_png, 480, 360))
    if overlays:
        _draw_text_xrgb(raw_data, 480, 360, overlays, scale=2)
    if out_png:
        os.makedirs(os.path.dirname(os.path.abspath(out_png)), exist_ok=True)
        encode_xrgb8888_to_png(bytes(raw_data), 480, 360, out_png)

    with open(output_raw, 'wb') as f:
        f.write(raw_data)

    print(f"[+] Successfully converted: {output_raw} ({len(raw_data)} bytes)")


if __name__ == "__main__":
    main()

