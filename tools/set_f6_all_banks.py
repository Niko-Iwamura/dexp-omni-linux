#!/usr/bin/env python3
"""Restore the sentinel edit, then write F6 -> PrintScreen into every NVRAM bank.

NVRAM layout (cmd 05/06), 0x200 apart, 3 bytes/slot:
  bank0 0x00C0, bank1 0x02C0, bank2 0x04C0
cmd 09 (the live-bank write) targets bank0 0xC0.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table, CHUNK

BANKS = {"bank0": 0x00C0, "bank1": 0x02C0, "bank2": 0x04C0}
A_KEY = 20   # slot of 'A'
F6_KEY = 6   # keyref index of F6


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


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    kc = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)[8 + 5]

    live = read_table(fd, kc, verbose=False)
    print(f"live cmd-07: slot{A_KEY}={live[A_KEY*3:A_KEY*3+3].hex(' ')}  "
          f"slot48(F6)={live[48*3:48*3+3].hex(' ')}")

    # base = live table; restore A key, set F6 -> PrintScreen (0x46)
    base = bytearray(live)
    base[A_KEY * 3:A_KEY * 3 + 3] = bytes([0x20, 0x00, 0x1D])   # A = HID 0x04? no: keep live value
    # Actually restore A from its original live value:
    base[A_KEY * 3:A_KEY * 3 + 3] = live[A_KEY * 3:A_KEY * 3 + 3]
    base[48 * 3:48 * 3 + 3] = bytes.fromhex("200046")

    print(f"\nwriting slot48 -> 20 00 46 into all banks "
          f"(and slot{A_KEY} back to {live[A_KEY*3:A_KEY*3+3].hex(' ')})")
    for name, off in BANKS.items():
        cur = bytearray(read_block(fd, off, kc * 3))
        cur[A_KEY * 3:A_KEY * 3 + 3] = live[A_KEY * 3:A_KEY * 3 + 3]
        cur[48 * 3:48 * 3 + 3] = bytes.fromhex("200046")
        ok = write_block(fd, off, bytes(cur))
        back = read_block(fd, off, kc * 3)
        print(f"  {name} @{off:#06x}: write_ok={ok}  "
              f"slot{A_KEY}={back[A_KEY*3:A_KEY*3+3].hex(' ')}  "
              f"slot48={back[48*3:48*3+3].hex(' ')}")

    live2 = read_table(fd, kc, verbose=False)
    print(f"\nlive cmd-07 after (no reload): slot48={live2[48*3:48*3+3].hex(' ')}")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)
    print("\nDone. Reload the keyboard (unplug/replug) then press F6.")


if __name__ == "__main__":
    main()
