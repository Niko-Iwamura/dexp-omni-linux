#!/usr/bin/env python3
"""Test: does the cmd-07 live table mirror NVRAM 0xC0 (cmd 05/06), not cmd 09?"""
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


def write_block(fd, off, data):
    total = len(data)
    pos = 0
    res = []
    while pos < total:
        n = min(CHUNK, total - pos)
        req = frame(0x06, data[pos:pos + n], off + pos)
        resp = transact(fd, req, verbose=False)
        res.append(None if resp is None else resp[7])
        pos += n
    return res


def main():
    slot = 48
    newval = bytes.fromhex("200046")
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    resp = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)
    kc = resp[8 + 5]

    live = read_table(fd, kc, verbose=False)
    nv = read_block(fd, 0x00C0, kc * 3)
    print(f"cmd07 slot{slot} = {live[slot*3:slot*3+3].hex(' ')}")
    print(f"nvram 0xC0 slot{slot} = {nv[slot*3:slot*3+3].hex(' ')}")

    print(f"\nwriting {newval.hex(' ')} to nvram 0x{0xC0+slot*3:03X} via cmd 06")
    tbl = bytearray(nv)
    tbl[slot * 3:slot * 3 + 3] = newval
    st = write_block(fd, 0x00C0, bytes(tbl))
    print(f"  cmd06 statuses: {[hex(s) for s in st if s is not None]}")

    nv2 = read_block(fd, 0x00C0, kc * 3)
    live2 = read_table(fd, kc, verbose=False)
    print(f"  nvram 0xC0 after = {nv2[slot*3:slot*3+3].hex(' ')}")
    print(f"  cmd07 after      = {live2[slot*3:slot*3+3].hex(' ')}")

    # restore
    print("\nrestoring original value via cmd 06")
    write_block(fd, 0x00C0, bytes(nv))
    nv3 = read_block(fd, 0x00C0, kc * 3)
    live3 = read_table(fd, kc, verbose=False)
    print(f"  nvram 0xC0 final = {nv3[slot*3:slot*3+3].hex(' ')}")
    print(f"  cmd07 final      = {live3[slot*3:slot*3+3].hex(' ')}")
    print(f"  restored: {nv3 == nv}")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
