#!/usr/bin/env python3
"""
patch_kernel_ipod.py - Binary-patch MT6572 kernel_mtk.bin for authentic iPod 5.5G USB mode.

Patches applied to uncompressed ARMv7 Image inside stock_unpacked/kernel_mtk.bin:
  1. Single LUN (0xc02eb294 in fsg_setup):
     Change `subeq r3, r3, #1` -> `moveq r3, #0` so US_BULK_GET_MAX_LUN returns 0 (1 LUN only).
     Eliminates the phantom second drive (LUN1) without touching bicr.
  2. INQUIRY CDB mask (0xc02faa10 in do_scsi_command):
     Change `mov r3, #0x10` -> `mov r3, #0x16` so check_command() allows EVPD (cmnd[1])
     and Page Code (cmnd[2]) in addition to Allocation Length (cmnd[4]).
  3. INQUIRY length clamp & EVPD hook (cave at 0xc05a12a0, end of .text):
     - Clamps common->data_size_from_cmnd to min(cmnd[4], common->data_size) before
       check_command() so iPodService.exe's INQUIRY (Cdb[4]=0xC0, DataTransferLength=0x60)
       never triggers a USB BOT Phase Error (Windows error 0x79 / ERROR_SEM_TIMEOUT).
     - When EVPD (cmnd[1] & 1) is set, dispatches to ipod_vpd_hook at 0xc05a12a0
       (registered by ipod_vpd.ko) to serve SCSI VPD pages 0x00, 0xC0, 0xC2..0xFF.
"""

import argparse
import gzip
import io
import os
import struct
import sys
import zlib

KERNEL_BASE = 0xc0008000

ADDR_GET_MAX_LUN = 0xc02eb3b4
ORIG_GET_MAX_LUN = 0x02433001  # subeq r3, r3, #1
NEW_GET_MAX_LUN  = 0x03a03000  # moveq r3, #0

ADDR_INQ_CLAMP   = 0xc02fab1c
ORIG_INQ_CLAMP   = 0xe5d4c05c  # ldrb ip, [r4, #0x5c]

ADDR_INQ_MASK    = 0xc02fab30
ORIG_INQ_MASK    = 0xe3a03010  # mov r3, #0x10
NEW_INQ_MASK     = 0xe3a0301e  # mov r3, #0x1e (allow cmnd[1..4]: EVPD, Page, 16-bit AllocLen)

ADDR_INQ_EVPD    = 0xc02fab54
ORIG_INQ_EVPD    = 0xe5dc3018  # ldrb r3, [ip, #0x18]

ADDR_CAVE_HOOK   = 0xc05a3a70
ADDR_CAVE_CLAMP  = 0xc05a3a74
ADDR_CAVE_EVPD   = 0xc05a3a90
ADDR_INQ_RESUME  = 0xc02fab58
ADDR_SCSI_DONE   = 0xc02f9cc0

# composite.c (bNumConfigurations = 2 + Config 2 Mass Storage + HID when ipod_vpd is loaded)
ADDR_DT_DEV_NUMCFG    = 0xc02f595c
ORIG_DT_DEV_NUMCFG    = 0xe5c60022  # strb r0, [r6, #0x22]
ADDR_DT_DEV_RESUME    = 0xc02f5960

ADDR_DT_CFG_NOTFOUND  = 0xc02f5c4c
ORIG_DT_CFG_NOTFOUND  = 0xe3a0a000  # mov sl, #0
ADDR_DT_CFG_NF_RESUME = 0xc02f5c54
ADDR_DT_CFG_BUILD     = 0xc02f5c80

ADDR_DT_SEND          = 0xc02f5620
ORIG_DT_SEND          = 0xe5980004  # ldr r0, [r8, #4]
ADDR_DT_SEND_RESUME   = 0xc02f5624


def arm_b(src_va: int, dst_va: int, link: bool = False, cond: int = 0xe) -> int:
    offset = (dst_va - (src_va + 8)) >> 2
    if not (-0x800000 <= offset <= 0x7fffff):
        raise ValueError(f"Branch out of range: {hex(src_va)} -> {hex(dst_va)}")
    op = 0x0b000000 if link else 0x0a000000
    return (cond << 28) | op | (offset & 0x00ffffff)


def build_cave(base_va: int):
    words = []
    # 0xc05a3a70: 4-byte function pointer slot (initially 0)
    words.append(0x00000000)

    # 0xc05a3a74: cave_clamp (16-bit big-endian Allocation Length in cmnd[3..4])
    words.append(0xe5d4c05b)  # ldrb ip, [r4, #0x5b]
    words.append(0xe5d4005c)  # ldrb r0, [r4, #0x5c]
    words.append(0xe180c40c)  # orr  ip, r0, ip, lsl #8
    words.append(0xe5940088)  # ldr  r0, [r4, #0x88]
    words.append(0xe15c0000)  # cmp  ip, r0
    words.append(0x81a0c000)  # movhi ip, r0
    words.append(0xe12fff1e)  # bx   lr

    # 0xc05a3a90: cave_evpd (dispatches all INQUIRY when ipod_vpd hook is loaded)
    assert base_va + len(words) * 4 == ADDR_CAVE_EVPD
    curr_pc = base_va + len(words) * 4 + 8
    hook_off = ADDR_CAVE_HOOK - curr_pc  # negative offset
    words.append(0xe51f3000 | (-hook_off & 0xfff))  # ldr r3, [pc, #-N]
    words.append(0xe3530000)  # cmp  r3, #0
    words.append(0x0a000007)  # beq  no_hook (+7*4)
    words.append(0xe2840058)  # add  r0, r4, #0x58
    words.append(0xe1a0100a)  # mov  r1, sl
    words.append(0xe5942088)  # ldr  r2, [r4, #0x88]
    words.append(0xe12fff33)  # blx  r3
    words.append(0xe3500000)  # cmp  r0, #0
    words.append(0xba000006)  # blt  vpd_invalid (+6*4)
    words.append(0xe1a06000)  # mov  r6, r0
    words.append(arm_b(base_va + len(words) * 4, ADDR_SCSI_DONE))  # b 0xc02f9cc0

    # no_hook:
    words.append(0xe5d43059)  # ldrb r3, [r4, #0x59]
    words.append(0xe3130001)  # tst  r3, #1
    words.append(0x1a000001)  # bne  vpd_invalid (+1*4)
    words.append(0xe5dc3018)  # ldrb r3, [ip, #0x18]
    words.append(arm_b(base_va + len(words) * 4, ADDR_INQ_RESUME))  # b 0xc02fab58

    # vpd_invalid:
    words.append(0xe594c074)  # ldr  ip, [r4, #0x74]
    words.append(0xe3023400)  # movw r3, #0x2400
    words.append(0xe3403005)  # movt r3, #5
    words.append(0xe58c301c)  # str  r3, [ip, #0x1c]
    words.append(0xe3e06015)  # mvn  r6, #0x15
    words.append(arm_b(base_va + len(words) * 4, ADDR_SCSI_DONE))  # b 0xc02f9cc0

    # cave_dt_dev: report bNumConfigurations = 2 when ipod_vpd hook is loaded
    # Must preserve both r1 (&cdev->desc) and APSR flags across 0xc02f595c!
    va_dt_dev = base_va + len(words) * 4
    words.append(0xe52d1004)  # push {r1} (save &cdev->desc)
    words.append(0xe10f1000)  # mrs  r1, apsr
    words.append(0xe52d1004)  # push {r1} (save APSR)
    curr_pc = base_va + len(words) * 4 + 8
    hook_off = ADDR_CAVE_HOOK - curr_pc
    words.append(0xe51f1000 | (-hook_off & 0xfff))  # ldr r1, [pc, #-N]
    words.append(0xe3510000)  # cmp  r1, #0
    words.append(0x0a000001)  # beq  +1*4 (skip cmp/moveq)
    words.append(0xe3500001)  # cmp  r0, #1
    words.append(0x03a00002)  # moveq r0, #2
    words.append(0xe49d1004)  # pop  {r1} (restore APSR)
    words.append(0xe129f001)  # msr  apsr_nzcvq, r1
    words.append(0xe49d1004)  # pop  {r1} (restore &cdev->desc)
    words.append(0xe5c60022)  # strb r0, [r6, #0x22]
    words.append(arm_b(base_va + len(words) * 4, ADDR_DT_DEV_RESUME))

    # cave_dt_cfg_nf: allow GET_DESCRIPTOR(CONFIG, index=1) using Config 1 template
    va_dt_cfg_nf = base_va + len(words) * 4
    curr_pc = base_va + len(words) * 4 + 8
    hook_off = ADDR_CAVE_HOOK - curr_pc
    words.append(0xe51f2000 | (-hook_off & 0xfff))  # ldr r2, [pc, #-N]
    words.append(0xe3520000)  # cmp  r2, #0
    words.append(0x0a000005)  # beq  nf_orig (+5*4)
    words.append(0xe20920ff)  # and  r2, r9, #0xff
    words.append(0xe3520001)  # cmp  r2, #1
    words.append(0x1a000002)  # bne  nf_orig (+2*4)
    words.append(0xe5962024)  # ldr  r2, [r6, #0x24]
    words.append(0xe242a01c)  # sub  sl, r2, #0x1c
    words.append(arm_b(base_va + len(words) * 4, ADDR_DT_CFG_BUILD))
    # nf_orig:
    words.append(0xe3a0a000)  # mov  sl, #0
    words.append(0xe3e07015)  # mvn  r7, #0x15
    words.append(arm_b(base_va + len(words) * 4, ADDR_DT_CFG_NF_RESUME))

    # cave_dt_send: at 0xc02f5620 (right before usb_ep_queue), set bConfigurationValue=2
    # on Config index 1 while keeping bNumInterfaces=1 (satisfies ITIPodValidateUSBConfiguration:
    # numConfigurations == 2 and configuration->bNumInterfaces == 1 || 3).
    va_dt_send = base_va + len(words) * 4
    curr_pc = base_va + len(words) * 4 + 8
    hook_off = ADDR_CAVE_HOOK - curr_pc
    words.append(0xe51f0000 | (-hook_off & 0xfff))  # ldr r0, [pc, #-N]
    words.append(0xe3500000)  # cmp  r0, #0
    words.append(0x0a000008)  # beq  send_done (+8*4)
    words.append(0xe1a00429)  # lsr  r0, r9, #8
    words.append(0xe3500002)  # cmp  r0, #2
    words.append(0x1a000005)  # bne  send_done (+5*4)
    words.append(0xe20900ff)  # and  r0, r9, #0xff
    words.append(0xe3500001)  # cmp  r0, #1
    words.append(0x1a000002)  # bne  send_done (+2*4)
    words.append(0xe5950000)  # ldr  r0, [r5, #0]   (buf = req->buf)
    words.append(0xe3a03002)  # mov  r3, #2
    words.append(0xe5c03005)  # strb r3, [r0, #5]   (bConfigurationValue = 2)
    # send_done:
    send_done_idx = len(words)
    words.append(0xe5980004)  # ldr  r0, [r8, #4]
    words.append(arm_b(base_va + len(words) * 4, ADDR_DT_SEND_RESUME))

    assert send_done_idx - ((va_dt_send - base_va) // 4) == 12
    cave = struct.pack(f"<{len(words)}I", *words)
    return cave, va_dt_dev, va_dt_cfg_nf, va_dt_send


def patch_kernel(in_path: str, out_path: str) -> None:
    with open(in_path, "rb") as f:
        raw = f.read()

    # Handle either 512-byte MTK header + zImage or raw zImage
    has_mtk_hdr = (raw[:4] == b"\x88\x16\x88\x58")
    mtk_hdr = raw[:512] if has_mtk_hdr else b""
    zimage = raw[512:] if has_mtk_hdr else raw

    gz_off = zimage.find(b"\x1f\x8b\x08")
    if gz_off < 0:
        raise RuntimeError("Could not find gzip stream inside zImage")

    decomp = zlib.decompressobj(16 + zlib.MAX_WBITS)
    image = bytearray(decomp.decompress(zimage[gz_off:]))
    tail_len = len(decomp.unused_data)
    if tail_len == 0:
        raise RuntimeError("Unexpected zImage layout (no trailer)")
    tail = zimage[-tail_len:]
    orig_gz_slot_len = len(zimage) - gz_off - tail_len

    def rd32(va: int) -> int:
        return struct.unpack_from("<I", image, va - KERNEL_BASE)[0]

    def wr32(va: int, val: int) -> None:
        struct.pack_into("<I", image, va - KERNEL_BASE, val)

    # Verify pristine opcodes
    checks = [
        (ADDR_GET_MAX_LUN,     ORIG_GET_MAX_LUN,     "GET_MAX_LUN"),
        (ADDR_INQ_CLAMP,       ORIG_INQ_CLAMP,       "INQ_CLAMP"),
        (ADDR_INQ_MASK,        ORIG_INQ_MASK,        "INQ_MASK"),
        (ADDR_INQ_EVPD,        ORIG_INQ_EVPD,        "INQ_EVPD"),
        (ADDR_DT_DEV_NUMCFG,   ORIG_DT_DEV_NUMCFG,   "DT_DEV_NUMCFG"),
        (ADDR_DT_CFG_NOTFOUND, ORIG_DT_CFG_NOTFOUND, "DT_CFG_NOTFOUND"),
        (ADDR_DT_SEND,         ORIG_DT_SEND,         "DT_SEND"),
    ]
    for va, expected, name in checks:
        actual = rd32(va)
        if actual != expected:
            raise RuntimeError(
                f"{name} @ {hex(va)}: expected {hex(expected)}, got {hex(actual)}"
            )

    cave_bytes, va_dt_dev, va_dt_cfg_nf, va_dt_send = build_cave(ADDR_CAVE_HOOK)
    cave_off = ADDR_CAVE_HOOK - KERNEL_BASE
    if any(b != 0 for b in image[cave_off : cave_off + len(cave_bytes)]):
        raise RuntimeError(f"Code cave at {hex(ADDR_CAVE_HOOK)} is not empty!")

    # Apply patches
    wr32(ADDR_GET_MAX_LUN, NEW_GET_MAX_LUN)
    wr32(ADDR_INQ_MASK, NEW_INQ_MASK)
    wr32(ADDR_INQ_CLAMP, arm_b(ADDR_INQ_CLAMP, ADDR_CAVE_CLAMP, link=True))
    wr32(ADDR_INQ_EVPD, arm_b(ADDR_INQ_EVPD, ADDR_CAVE_EVPD, link=False))
    wr32(ADDR_DT_DEV_NUMCFG, arm_b(ADDR_DT_DEV_NUMCFG, va_dt_dev, link=False))
    wr32(ADDR_DT_CFG_NOTFOUND, arm_b(ADDR_DT_CFG_NOTFOUND, va_dt_cfg_nf, link=False))
    wr32(ADDR_DT_SEND, arm_b(ADDR_DT_SEND, va_dt_send, link=False))
    image[cave_off : cave_off + len(cave_bytes)] = cave_bytes

    # Lower lowest mt_cpufreq OPP [4] (598000 KHz @ 1150 mV -> 390000 KHz @ 1050 mV)
    # for longer screen-off music playback battery life.
    opp_old = struct.pack("<IIII", 806000, 1150, 598000, 1150)
    opp_new = struct.pack("<IIII", 806000, 1150, 390000, 1050)
    opp_hits = 0
    search_pos = 0
    while True:
        idx = image.find(opp_old, search_pos)
        if idx < 0:
            break
        image[idx : idx + len(opp_old)] = opp_new
        opp_hits += 1
        search_pos = idx + len(opp_old)

    # Recompress with gzip -9 and pad to exact original slot size
    gz_buf = io.BytesIO()
    with gzip.GzipFile(fileobj=gz_buf, mode="wb", compresslevel=9, mtime=0) as gz:
        gz.write(image)
    new_gz = gz_buf.getvalue()

    if len(new_gz) > orig_gz_slot_len:
        raise RuntimeError(
            f"Recompressed kernel ({len(new_gz)} B) exceeds original slot ({orig_gz_slot_len} B)"
        )

    pad_len = orig_gz_slot_len - len(new_gz)
    if pad_len == 0:
        padded_gz = new_gz
    elif pad_len >= 4:
        # ARM zImage head.S reads (input_data_end - 4) for the uncompressed size
        # (gzip ISIZE) to detect overlap and relocate zImage before decompression.
        padded_gz = new_gz + b"\x00" * (pad_len - 4) + struct.pack("<I", len(image))
    else:
        raise RuntimeError(
            f"Unsupported zImage padding gap ({pad_len} B) for ISIZE trailer"
        )
    new_zimage = zimage[:gz_off] + padded_gz + tail
    assert len(new_zimage) == len(zimage)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(mtk_hdr + new_zimage)

    print(
        f"[patch_kernel_ipod] Patched {out_path} "
        f"(gz {len(new_gz)}/{orig_gz_slot_len} B, cave {len(cave_bytes)} B)"
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="Input stock_unpacked/kernel_mtk.bin")
    ap.add_argument("output", help="Output patched kernel_ipod_mtk.bin")
    args = ap.parse_args()
    patch_kernel(args.input, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
