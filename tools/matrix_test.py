#!/usr/bin/env python3
"""Empirical matrix: which channel actually persists key assignments on 320F:505B."""
import os
import select
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_cmd09 import find_node, frame, transact, read_table, CHUNK


def write_with(fd, cmd, table):
    total = len(table)
    off = 0
    statuses = []
    while off < total:
        n = min(CHUNK, total - off)
        req = frame(cmd, table[off:off + n], off)
        resp = transact(fd, req, verbose=False)
        if resp is None:
            return f"no answer at off {off:#06x}"
        statuses.append(resp[7])
        off += n
    bad = [s for s in statuses if s in (0xFF, 0xFE)]
    return f"statuses={[hex(s) for s in set(statuses)]}" + (
        f" REJECTED({len(bad)} chunks)" if bad else " all-OK")


def open_dev():
    node = find_node()
    if not node:
        raise SystemExit("no config interface")
    return os.open(node, os.O_RDWR), node


def run(label, cmd, changed, do_commit=True, pulse=False, commit_first=False,
        read_before_commit=False):
    fd, node = open_dev()
    transact(fd, frame(0x01), verbose=False)
    resp = transact(fd, frame(0x03, b"\x00" * 24), verbose=False)
    kc = resp[8 + 5]
    before = read_table(fd, kc, verbose=False)
    tbl = bytearray(before)
    if changed:
        tbl[48 * 3:48 * 3 + 3] = bytes.fromhex("200046")
    res = write_with(fd, cmd, bytes(tbl))

    mid = None
    if read_before_commit:
        mid = read_table(fd, kc, verbose=False)[48 * 3:48 * 3 + 3].hex(" ")
    if commit_first:
        transact(fd, frame(0x02), verbose=False)
        transact(fd, frame(0x01), verbose=False)
    if pulse:
        transact(fd, frame(0x1C), verbose=False)
    if do_commit:
        transact(fd, frame(0x02), verbose=False)

    after = read_table(fd, kc, verbose=False)
    os.close(fd)
    print(f"{label:<46} write={res}")
    if mid is not None:
        print(f"{'':46} slot48 pre-commit = {mid}")
    print(f"{'':46} slot48 before={before[48*3:48*3+3].hex(' ')}  "
          f"after={after[48*3:48*3+3].hex(' ')}  "
          f"{'PERSISTED' if after != before else 'unchanged'}")


print("=== A: cmd 09, changed, read before commit, then commit ===")
run("A cmd09 changed", 0x09, True, read_before_commit=True)

print("\n=== B: cmd 09, changed, NO commit then read (fresh open used for read) ===")
run("B cmd09 no-commit", 0x09, True, do_commit=False)

print("\n=== C: cmd 27 (second bank), changed, commit ===")
run("C cmd27 changed", 0x27, True)

print("\n=== D: cmd 09, write table UNCHANGED ===")
run("D cmd09 unchanged", 0x09, False)

print("\n=== E: cmd 09, changed, commit then begin again ===")
run("E cmd09 commit-then-begin", 0x09, True, commit_first=True)

print("\n=== F: cmd 09, changed, pulse cmd 1C ===")
run("F cmd09 + pulse 1C", 0x09, True, pulse=True)
