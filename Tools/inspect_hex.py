# -*- coding: utf-8 -*-
"""Inspect Intel HEX record types and main-Flash address ranges."""
import collections
import sys


def parse_hex(path):
    records = []
    ext = 0
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            n = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            typ = int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + 2 * n])
            if typ == 0x04:
                ext = int(line[9:13], 16) << 16
            elif typ == 0x00:
                records.append((ext + addr, data))
            elif typ == 0x01:
                break
    return records


for tag, path in [("BL", sys.argv[1]), ("APP", sys.argv[2])]:
    recs = parse_hex(path)
    lo = min(a for a, _ in recs)
    hi = max(a + len(d) for a, d in recs)
    print("%s: data records=%d range=0x%08X ~ 0x%08X" % (tag, len(recs), lo, hi))
    # group by 16-bit page to spot high config region
    pages = collections.defaultdict(int)
    for a, d in recs:
        pages[a >> 16] += len(d)
    for page in sorted(pages):
        print("   segment 0x%04X0000 size=%d" % (page, pages[page]))
