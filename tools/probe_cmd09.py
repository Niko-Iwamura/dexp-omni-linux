#!/usr/bin/env python3
"""Minimal cmd 07/09 prober for 320F:505B - prints every frame and status.

Reads the live assignment table (cmd 07), optionally rewrites one slot via
cmd 09, then reads back. Used to debug why cmd 09 writes appear not to persist.
"""
import argparse
import select
import sys

REPORT = 0x40
CHUNK = 0x38


def find_node():
    import glob
    import os
    # /dev/hidrawN is a real device node, not a symlink: resolve through sysfs,
    # matching the char device number.
    by_rdev = {}
    for sysdir in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            with open(os.path.join(sysdir, "dev")) as f:
                maj, minr = (int(x) for x in f.read().split(":"))
        except (OSError, ValueError):
            continue
        by_rdev[(maj, minr)] = sysdir
    for path in sorted(glob.glob("/dev/hidraw*"),
                       key=lambda p: int(p.rsplit("hidraw", 1)[1])):
        try:
            dev = os.stat(path).st_rdev
        except OSError:
            continue
        sysdir = by_rdev.get((os.major(dev), os.minor(dev)))
        if not sysdir:
            continue
        hiddir = os.path.realpath(os.path.join(sysdir, "device"))
        if "320F:505B" not in hiddir.upper():
            continue
        desc = os.path.join(hiddir, "report_descriptor")
        try:
            with open(desc, "rb") as f:
                d = f.read()
        except OSError:
            continue
        if b"\x85\x04" in d and b"\x06\x1c\xff" in d.lower():
            return path
    return None


def checksum(buf):
    return sum(buf[3:64]) & 0xFFFF


def frame(cmd, payload=b"", off=0):
    buf = bytearray(REPORT)
    buf[0] = 0x04
    buf[3] = cmd
    buf[4] = len(payload)
    buf[5] = off & 0xFF
    buf[6] = (off >> 8) & 0xFF
    buf[8:8 + len(payload)] = payload
    cs = checksum(buf)
    buf[1] = cs & 0xFF
    buf[2] = (cs >> 8) & 0xFF
    return bytes(buf)


def transact(fd, req, verbose=True):
    import os
    os.write(fd, req)
    while True:
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            return None
        resp = os.read(fd, REPORT)
        if len(resp) < 8 or resp[0] != 0x04:
            if verbose:
                print(f"  skip report id {resp[0]:#04x} ({len(resp)}B)")
            continue
        if resp[3] != req[3]:
            if verbose:
                print(f"  skip cmd {resp[3]:#04x} (wanted {req[3]:#04x})")
            continue
        if verbose:
            print(f"  resp status={resp[7]:#04x}  {resp[:16].hex(' ')}")
        return resp


def read_table(fd, kc, verbose=True):
    total = kc * 3
    out = bytearray()
    off = 0
    while off < total:
        n = min(CHUNK, total - off)
        req = frame(0x07, b"\x00" * n, off)
        resp = transact(fd, req, verbose)
        if resp is None:
            raise SystemExit(f"no answer at offset {off}")
        if resp[7] in (0xFF, 0xFE):
            raise SystemExit(f"read error at offset {off}: status {resp[7]:#04x}")
        out += resp[8:8 + n]
        off += n
    return bytes(out)


def write_table(fd, table, verbose=True):
    total = len(table)
    off = 0
    while off < total:
        n = min(CHUNK, total - off)
        req = frame(0x09, table[off:off + n], off)
        print(f"  TX cmd09 off={off:#06x} len={n}")
        resp = transact(fd, req, verbose)
        if resp is None:
            raise SystemExit(f"no answer at offset {off}")
        if resp[7] in (0xFF, 0xFE):
            print(f"  !! write rejected at offset {off}: status {resp[7]:#04x}")
        off += n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--node")
    ap.add_argument("--slot", type=int, default=48)
    ap.add_argument("--value", default="20 00 46",
                    help='3 hex bytes, e.g. "20 00 46"')
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--pulse", action="store_true", help="send cmd 1C after write")
    ap.add_argument("--commit", action="store_true", help="send cmd 02 after write")
    args = ap.parse_args()

    import os
    node = args.node or find_node()
    if not node:
        raise SystemExit("no 320F:505B config interface found")
    fd = os.open(node, os.O_RDWR)
    print(f"node {node}")

    print("cmd 01 (begin)")
    transact(fd, frame(0x01))

    print("cmd 03 (info)")
    resp = transact(fd, frame(0x03, b"\x00" * 24))
    kc = resp[8 + 5] if resp else 0
    print(f"  keycount = {kc}")

    print(f"cmd 07 (read table, {kc} slots)")
    before = read_table(fd, kc)
    print(f"  slot {args.slot}: {before[args.slot*3:args.slot*3+3].hex(' ')}")

    if args.write:
        val = bytes(int(b, 16) for b in args.value.split())
        assert len(val) == 3, "need exactly 3 bytes"
        tbl = bytearray(before)
        tbl[args.slot * 3:args.slot * 3 + 3] = val
        print(f"cmd 09 (write table, slot {args.slot} -> {val.hex(' ')})")
        write_table(fd, bytes(tbl))

        if args.commit:
            print("cmd 02 (commit)")
            transact(fd, frame(0x02))
        if args.pulse:
            print("cmd 1C (pulse)")
            transact(fd, frame(0x1C))

        print("cmd 07 (read back, same session)")
        now = read_table(fd, kc)
        print(f"  slot {args.slot}: {now[args.slot*3:args.slot*3+3].hex(' ')}")

    if not args.commit and not args.pulse:
        print("cmd 02 (commit)")
        transact(fd, frame(0x02))

    print("cmd 07 (read back, after commit)")
    final = read_table(fd, kc)
    print(f"  slot {args.slot}: {final[args.slot*3:args.slot*3+3].hex(' ')}")
    os.close(fd)


if __name__ == "__main__":
    main()
