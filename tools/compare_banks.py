#!/usr/bin/env python3
"""Compare the three NVRAM assignment banks against the live cmd-07 table."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table, CHUNK


def read_block(fd, off, n):
    out = bytearray()
    pos = 0
    while pos < n:
        c = min(CHUNK, n - pos)
        req = frame(0x05, b"\x00" * c, off + pos)
        resp = transact(fd, req, verbose=False)
        if resp is None or resp[7] in (0xFF, 0xFE):
            return None
        out += resp[8:8 + c]
        pos += c
    return bytes(out)


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    resp = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)
    kc = resp[8 + 5]
    live = read_table(fd, kc, verbose=False)
    print(f"kc={kc}, live cmd-07 slot48={live[48*3:48*3+3].hex(' ')}\n")

    for name, base in [("bank0", 0x00C0), ("bank1", 0x02C0), ("bank2", 0x04C0)]:
        blk = read_block(fd, base, kc * 3)
        if blk is None:
            print(f"{name} @{base:#06x}: read failed")
            continue
        diff = [i for i in range(kc) if blk[i*3:i*3+3] != live[i*3:i*3+3]]
        print(f"{name} @{base:#06x}: slot48={blk[48*3:48*3+3].hex(' ')}  "
              f"slots differing from live: {len(diff)} {diff[:12]}")

    # also dump the settings record + active index
    idx = read_block(fd, 0x0000, 1)
    print(f"\nactive settings-slot index P = {idx[0] if idx else '?'}")
    rec = read_block(fd, 0x0001 + idx[0] * 0x21, 0x22) if idx else None
    if rec:
        print(f"settings record: {rec.hex(' ')}")
        print(f"  mode={rec[0]} brightness={rec[1]} speed={rec[2]} "
              f"custom_group={rec[25] if len(rec) > 25 else '?'}")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
