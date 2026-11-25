#!/usr/bin/env python3
"""Can anything make the device re-read its assignment bank without a replug?

Writes F6 -> PrintScreen into NVRAM 0xC0, then tries several 'nudge' commands
and after each one re-reads the live cmd-07 table.
Restores the original bank0 table at the end.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table, CHUNK

BANK0 = 0x00C0
F6 = 48
NEW = bytes.fromhex("200046")


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


def live48(fd, kc):
    return read_table(fd, kc, verbose=False)[F6 * 3:F6 * 3 + 3].hex(" ")


def nudge(fd, kc, label, fn):
    fn()
    print(f"  after {label:<34} live48 = {live48(fd, kc)}")


def main():
    node = find_node()
    fd = os.open(node, os.O_RDWR)
    transact(fd, frame(0x01), verbose=False)
    kc = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)[8 + 5]

    orig = read_block(fd, BANK0, kc * 3)
    print(f"bank0 slot48 orig = {orig[F6*3:F6*3+3].hex(' ')}")
    print(f"live  slot48      = {live48(fd, kc)}\n")

    t = bytearray(orig)
    t[F6 * 3:F6 * 3 + 3] = NEW
    write_block(fd, BANK0, bytes(t))
    print(f"wrote bank0 slot48 -> {NEW.hex(' ')}")
    print(f"  immediate                        live48 = {live48(fd, kc)}")

    idx = read_block(fd, 0x0000, 1)[0]
    rec_off = 0x0001 + idx * 0x21
    rec = read_block(fd, rec_off, 0x22)

    def rewrite_rec():
        write_block(fd, rec_off, rec)

    def toggle_mode():
        r = bytearray(rec)
        r[1] = (r[1] + 1) % 31
        write_block(fd, rec_off, bytes(r))
        write_block(fd, rec_off, rec)

    def session_bounce():
        transact(fd, frame(0x02), verbose=False)
        transact(fd, frame(0x01), verbose=False)

    def pulse():
        transact(fd, frame(0x1C), verbose=False)

    def probe():
        transact(fd, frame(0xAA), verbose=False)

    nudge(fd, kc, "settings record rewrite (cmd 06)", rewrite_rec)
    nudge(fd, kc, "mode toggle + restore", toggle_mode)
    nudge(fd, kc, "cmd 02 + cmd 01", session_bounce)
    nudge(fd, kc, "cmd 1C pulse", pulse)
    nudge(fd, kc, "cmd AA probe", probe)

    print("\nrestoring bank0")
    write_block(fd, BANK0, orig)
    back = read_block(fd, BANK0, kc * 3)
    print(f"  bank0 slot48 = {back[F6*3:F6*3+3].hex(' ')} (orig {orig[F6*3:F6*3+3].hex(' ')})")

    transact(fd, frame(0x02), verbose=False)
    os.close(fd)


if __name__ == "__main__":
    main()
