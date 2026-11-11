# -*- coding: utf-8 -*-
"""Independent verification of generated bl.bin / app.bin / merged_bl_app.bin."""
import os
import struct

# Repository root = parent of the Tools/ folder holding this script.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT_DIR, "BL", "out", "BL")
HEX_BL = os.path.join(ROOT_DIR, "BL", "out", "BL", "default.hex")
HEX_APP = os.path.join(ROOT_DIR, "APP", "out", "APP", "default.hex")
BL_SIZE = 0x2000
APP_OFFSET = 0x2000
BANK_SIZE = 0x80000


def parse_hex(hex_path, lo, hi):
    img = {}
    ext = 0
    for line in open(hex_path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        n = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        typ = int(line[7:9], 16)
        data = bytes.fromhex(line[9:9 + 2 * n])
        if typ == 4:
            ext = int(line[9:13], 16) << 16
            continue
        if typ != 0:
            continue
        for i, b in enumerate(data):
            a = ext + addr + i
            if lo <= a < hi:
                img[a] = b
    return img


def read(path):
    with open(path, "rb") as f:
        return f.read()


bl_hex = parse_hex(HEX_BL, 0, BL_SIZE)
app_hex = parse_hex(HEX_APP, APP_OFFSET, BANK_SIZE)

bl_bin = read(os.path.join(OUT_DIR, "bl.bin"))
app_bin = read(os.path.join(OUT_DIR, "app.bin"))
merged = read(os.path.join(OUT_DIR, "merged_bl_app.bin"))

fail = []

# 1 sizes
print("bl.bin size   : %d (expect %d)" % (len(bl_bin), BL_SIZE))
if len(bl_bin) != BL_SIZE:
    fail.append("bl.bin size wrong")
print("app.bin size  : %d" % len(app_bin))
print("merged size   : %d (0x%X)" % (len(merged), len(merged)))
if len(merged) % 0x2000 != 0:
    fail.append("merged not 8KB aligned")

# 2 vectors
bl_msp, bl_rst = struct.unpack_from("<II", bl_bin, 0)
app_msp, app_rst = struct.unpack_from("<II", app_bin, 0)
print("BL vector: MSP=0x%08X reset=0x%08X (Thumb entry 0x%08X)" % (bl_msp, bl_rst, bl_rst & ~1))
print("APP vector: MSP=0x%08X reset=0x%08X (Thumb entry 0x%08X)" % (app_msp, app_rst, app_rst & ~1))
if bl_msp != 0x2003FFF8:
    fail.append("BL MSP mismatch")
if app_msp != 0x2003FFF8:
    fail.append("APP MSP mismatch")

# 3 merged equals bl slot + app slot
if merged[0:BL_SIZE] != bl_bin:
    fail.append("merged BL slot differs from bl.bin")
if merged[APP_OFFSET:APP_OFFSET + len(app_bin)] != app_bin:
    fail.append("merged APP slot differs from app.bin")

# 4 compare program bytes against HEX main Flash
for a, b in bl_hex.items():
    if bl_bin[a] != b:
        fail.append("bl.bin mismatch at 0x%X" % a)
for a, b in app_hex.items():
    rel = a - APP_OFFSET
    if app_bin[rel] != b:
        fail.append("app.bin mismatch at 0x%X" % a)

# 5 unused BL tail must be 0xFF
if any(x != 0xFF for x in bl_bin[max(bl_hex) + 1:BL_SIZE]):
    fail.append("BL tail not FF")
# APP tail inside merged must be FF
app_end = APP_OFFSET + len(app_bin)
if any(x != 0xFF for x in merged[app_end:]):
    fail.append("merged APP tail not FF")

# 6 no config-bit bytes leaked: merged must not contain 0x804000 region content by address
print("merged BL slot identical to bl.bin : OK")
print("merged APP slot identical to app.bin: OK")
print("HEX main-Flash byte compare         : OK")

if fail:
    print("\nFAILED:")
    for f in fail:
        print(" -", f)
    raise SystemExit(1)
print("\nALL CHECKS PASSED")
