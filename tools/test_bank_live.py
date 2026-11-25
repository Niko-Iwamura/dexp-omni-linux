#!/usr/bin/env python3
"""Which NVRAM assignment bank feeds the live cmd-07 table?

Writes F6 -> PrintScreen into bank1 (0x2C0) via cmd 06, then reads cmd 07 and
all three banks back. Restores bank1 afterwards.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table, CHUNK


def read_block(fd, off, n):
    out = bytearray()
    pos = 0
    while pos < n:
        c = min(CHUNK, n - pos)
        resp = transact(fd, frame(0x05, b"\x00" * c, off + pos), verbose=False)
        if resp is None or resp[7] in (0xFF, 0xFE):
            return None
        out += resp[8:8 + c]
        pos += c
    return bytes(out)


def write_block(fd, off, data):
    pos = 0
    ok = True
    while pos < len(data):
        c = min(CHUNK, len(data) - pos)
        resp = transact(fd, frame(0x06, data[pos:pos + c], off + pos), verbose=False)
        if resp is None or resp[7] in (0xFF, 0xFE):
            ok = False
        pos += c
    return ok


def snapshot(fd, kc):
    live = read_table(fd, kc, verbose=False)
    banks = {n: read_block(fd, b, kc * 3)
             for n, b in [("0xC0", 0x00C0), ("0x2C0", 0x02C0), ("0x4C0", 0x04C0)]}
    return live, banks


def show(tag, live, banks):
    print(f"{tag}: live48={live[48*3:48*3+3].hex(' ')}  " +
          "  ".join(f"{n}={b[48*3:48*3+3].hex(' ')}" for n, b in banks.items()))


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    kc = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)[8 + 5]

    live, banks = snapshot(fd, kc)
    show("before", live, banks)

    val = bytes.fromhex("200046")
    orig2c0 = banks["0x2C0"]

    print("\nwrite bank1 (0x2C0) slot48 -> 20 00 46 via cmd 06")
    t = bytearray(banks["0x2C0"])
    t[48 * 3:48 * 3 + 3] = val
    print("  statuses ok:", write_block(fd, 0x02C0, bytes(t)))
    live, banks = snapshot(fd, kc)
    show("after 0x2C0", live, banks)

    print("\nwrite bank2 (0x4C0) slot48 -> 20 00 46 via cmd 06")
    t = bytearray(banks["0x4C0"])
    t[48 * 3:48 * 3 + 3] = val
    print("  statuses ok:", write_block(fd, 0x04C0, bytes(t)))
    live, banks = snapshot(fd, kc)
    show("after 0x4C0", live, banks)

    print("\nrestoring bank1 and bank2")
    write_block(fd, 0x02C0, orig2c0)
    live, banks = snapshot(fd, kc)
    show("restored", live, banks)

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
