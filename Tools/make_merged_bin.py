# -*- coding: utf-8 -*-
"""
Build the Fail-Safe Dual Bank merged image for PIC32CX1025SG41128.

Layout of merged_bl_app.bin (written by btl_host.py to inactive Bank base 0x80000):
  file offset 0x0000 ~ 0x1FFF : BL image (Bank-internal offset 0), padded with 0xFF to 8 KB
  file offset 0x2000 ~ ...    : APP image (Bank-internal offset 0x2000), padded with 0xFF

Config-bit records at 0x804000 are deliberately excluded: this OTA flow does not
program device configuration bits (BTL_FUSE_PROGRAM_ENABLE is disabled).

Inputs are the toolchain-generated Intel HEX files. The previous raw bins produced
by a plain objcopy were ~8 MB because the 0x804000 config segment and RAM segments
were expanded into the binary; this script extracts only the main Flash segments.
"""

import os
import sys

# Flash partition facts (must match bootloader_common.h / APP linker macros)
BL_SIZE = 0x2000          # 8192 bytes bootloader slot inside one Bank
APP_OFFSET = 0x2000       # Bank-internal APP start offset
BANK_SIZE = 0x80000       # 512 KB per Bank
ERASE_BLOCK = 0x2000      # 8192 byte NVMCTRL erase block
CONFIG_BASE = 0x800000    # config-bit region (0x804000...) excluded from image
FLASH_END = 0x100000

# Repository root = parent of the Tools/ folder holding this script.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEX_BL = os.path.join(ROOT_DIR, "BL", "out", "BL", "default.hex")
HEX_APP = os.path.join(ROOT_DIR, "APP", "out", "APP", "default.hex")
OUT_DIR = os.path.join(ROOT_DIR, "BL", "out", "BL")
OUT_BL_BIN = os.path.join(OUT_DIR, "bl.bin")
OUT_APP_BIN = os.path.join(OUT_DIR, "app.bin")
OUT_MERGED = os.path.join(OUT_DIR, "merged_bl_app.bin")


def parse_hex(hex_path, address_filter):
    """Return {absolute_address: byte} for data records accepted by filter."""
    image = {}
    ext = 0
    with open(hex_path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":"):
                continue
            n = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            typ = int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + 2 * n])
            if typ == 0x04:
                ext = int(line[9:13], 16) << 16
                continue
            if typ != 0x00:
                continue
            base = ext + addr
            for i, byte in enumerate(data):
                abs_addr = base + i
                if address_filter(abs_addr):
                    image[abs_addr] = byte
    return image


def segment_to_bytes(image, start, end, pad=0xFF):
    """Compact bytes for [start, end); missing addresses are pad bytes."""
    out = bytearray()
    for addr in range(start, end):
        out.append(image.get(addr, pad))
    return bytes(out)


def align_up(size, alignment):
    return (size + alignment - 1) & ~(alignment - 1)


def main():
    # BL main Flash: [0, BL_SIZE); skip everything >= 0x800000
    bl_img = parse_hex(HEX_BL, lambda a: a < BL_SIZE)
    bl_lo = min(bl_img)
    bl_hi = max(bl_img) + 1
    bl_program = segment_to_bytes(bl_img, bl_lo, bl_hi)

    # APP main Flash: [0x2000, BANK_SIZE); skip config region
    app_img = parse_hex(HEX_APP, lambda a: APP_OFFSET <= a < BANK_SIZE)
    app_lo = min(app_img)
    app_hi = max(app_img) + 1
    app_program = segment_to_bytes(app_img, app_lo, app_hi)

    if app_lo != APP_OFFSET:
        print("ERROR: APP image does not start at 0x2000, got 0x%X" % app_lo)
        sys.exit(1)

    # BL raw bin: Bank-internal offset 0 start, padded to 8 KB
    bl_bin = bytearray([0xFF]) * BL_SIZE
    bl_bin[0:len(bl_program)] = bl_program
    with open(OUT_BL_BIN, "wb") as fh:
        fh.write(bl_bin)

    # APP raw bin: compact, Bank-internal offset 0x2000 becomes file offset 0
    app_bin = bytearray(app_program)
    with open(OUT_APP_BIN, "wb") as fh:
        fh.write(app_bin)

    # Merged image: BL at file offset 0, APP at file offset APP_OFFSET
    merged_len = align_up(APP_OFFSET + len(app_bin), ERASE_BLOCK)
    merged = bytearray([0xFF]) * merged_len
    merged[0:BL_SIZE] = bl_bin
    merged[APP_OFFSET:APP_OFFSET + len(app_bin)] = app_bin
    with open(OUT_MERGED, "wb") as fh:
        fh.write(merged)

    # Verification report
    def head8(buf, off=0):
        return " ".join("%02X" % b for b in buf[off:off + 8])

    print("== BL image ==")
    print("  program range : 0x%08X ~ 0x%08X (%d bytes)" % (bl_lo, bl_hi, len(bl_program)))
    print("  bl.bin        : %d bytes (padded to %d)" % (len(bl_bin), BL_SIZE))
    print("  BL vector head: %s" % head8(bl_bin, 0))

    print("== APP image ==")
    print("  program range : 0x%08X ~ 0x%08X (%d bytes)" % (app_lo, app_hi, len(app_program)))
    print("  app.bin       : %d bytes" % len(app_bin))
    print("  APP vector head: %s" % head8(app_bin, 0))

    print("== merged_bl_app.bin ==")
    print("  total size    : %d bytes (0x%X)" % (len(merged), len(merged)))
    print("  BL head @0x0000: %s" % head8(merged, 0))
    print("  APP head @0x2000: %s" % head8(merged, APP_OFFSET))
    print("  output dir    : %s" % OUT_DIR)

    # Boundary sanity checks
    assert len(bl_bin) == BL_SIZE, "BL slot must be exactly 8 KB"
    assert app_hi <= BANK_SIZE, "APP crosses Bank boundary"
    assert APP_OFFSET + len(app_bin) <= BANK_SIZE, "APP overflows Bank"
    print("  boundary check: OK")


if __name__ == "__main__":
    main()
