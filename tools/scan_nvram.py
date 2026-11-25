#!/usr/bin/env python3
"""Scan the cmd 05 settings-NVRAM space for the key-assignment table signature.

The cmd-07 table starts with the Esc/backtick/Tab triples:
 20 00 29 20 00 35 20 00 2b 20 00 39 ...
If that byte run appears at some offset, cmd 05/06 (not cmd 09) may be the
real storage for assignments on 320F:505B.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table


SIG = bytes.fromhex("20 00 29 20 00 35 20 00 2b".replace(" ", ""))


def read_block(fd, off, n):
    req = frame(0x05, b"\x00" * n, off)
    resp = transact(fd, req, verbose=False)
    if resp is None or resp[7] in (0xFF, 0xFE):
        return None
    return resp[8:8 + n]


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)

    # live table for comparison
    resp = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)
    kc = resp[8 + 5]
    live = read_table(fd, kc, verbose=False)
    print(f"live cmd-07 table[0:12] = {live[:12].hex(' ')}")
    print(f"searching for {SIG.hex(' ')} in cmd-05 space 0x0000..0x1FFF\n")

    hits = []
    off = 0
    while off < 0x2000:
        blk = read_block(fd, off, 0x38)
        if blk is None:
            print(f"  read failed at {off:#06x}")
            break
        idx = blk.find(SIG)
        if idx >= 0:
            hits.append(off + idx)
        off += 0x38
    if hits:
        for h in hits:
            print(f"  HIT at {h:#06x}")
            # verify by reading 48 bytes around it
            start = h
            got = read_block(fd, start, 0x30)
            print(f"    {start:#06x}: {got.hex(' ')}")
    else:
        print("  no match in 0x0000..0x1FFF")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
