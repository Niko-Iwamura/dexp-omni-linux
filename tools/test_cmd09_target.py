#!/usr/bin/env python3
"""Sentinel test: does cmd 09 write into NVRAM 0xC0? And restore stray edits."""
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
    while pos < len(data):
        c = min(CHUNK, len(data) - pos)
        transact(fd, frame(0x06, data[pos:pos + c], off + pos), verbose=False)
        pos += c


def write_cmd09(fd, table):
    pos = 0
    st = set()
    while pos < len(table):
        c = min(CHUNK, len(table) - pos)
        resp = transact(fd, frame(0x09, table[pos:pos + c], pos), verbose=False)
        st.add(None if resp is None else resp[7])
        pos += c
    return [hex(s) for s in st if s is not None]


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    kc = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)[8 + 5]

    # repair the stray bank2 edit from the previous run
    b2 = bytearray(read_block(fd, 0x04C0, kc * 3))
    if b2[48 * 3:48 * 3 + 3] == bytes.fromhex("200046"):
        b2[48 * 3:48 * 3 + 3] = bytes.fromhex("20003f")
        write_block(fd, 0x04C0, bytes(b2))
        print("repaired bank2 slot48 -> 20 00 3f")
    print(f"bank2 slot48 now: {read_block(fd, 0x04C0, kc*3)[48*3:48*3+3].hex(' ')}")

    live = read_table(fd, kc, verbose=False)
    b0 = read_block(fd, 0x00C0, kc * 3)
    print(f"\nbefore: live48={live[48*3:48*3+3].hex(' ')}  "
          f"nvram0xC0[48]={b0[48*3:48*3+3].hex(' ')}  "
          f"nvram0xC0[20]={b0[20*3:20*3+3].hex(' ')}")

    # sentinel: set slot 20 (A key) to PrintScreen via cmd 09
    SENT = bytes.fromhex("200046")
    t = bytearray(live)
    t[20 * 3:20 * 3 + 3] = SENT
    print(f"\ncmd09: slot20 -> {SENT.hex(' ')}")
    print(f"  statuses: {write_cmd09(fd, bytes(t))}")

    live2 = read_table(fd, kc, verbose=False)
    b0b = read_block(fd, 0x00C0, kc * 3)
    print(f"\nafter : live20={live2[20*3:20*3+3].hex(' ')}  "
          f"nvram0xC0[20]={b0b[20*3:20*3+3].hex(' ')}")
    print(f"        live48={live2[48*3:48*3+3].hex(' ')}  "
          f"nvram0xC0[48]={b0b[48*3:48*3+3].hex(' ')}")

    if live2[20 * 3:20 * 3 + 3] != SENT:
        print("\n=> live table did NOT change (cmd 09 inert for reads)")
    if b0b[20 * 3:20 * 3 + 3] == SENT:
        print("=> NVRAM 0xC0 DID change: cmd 09 writes into bank 0xC0")
    else:
        print("=> NVRAM 0xC0 unchanged: cmd 09 does not target 0xC0")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
