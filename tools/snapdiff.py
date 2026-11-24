#!/usr/bin/env python3
"""Compare two `omni-cli snapshot` files and say what changed.

    omni-cli snapshot before.txt      # then change something in the Windows app
    omni-cli snapshot after.txt
    tools/snapdiff.py before.txt after.txt

Regions known for USB 320F:505B (PROTOCOL.md 5.2b, 13) are decoded: the active
settings record, the three custom-colour groups and the macro image. The cmd 0A
space mirrors the cmd 05 space shifted by 0x6C0, so changes seen in both are
reported once.
"""
import sys

REC = {0: "slot", 1: "mode", 2: "brightness", 3: "speed", 4: "fx", 5: "multicolor",
       6: "R", 7: "G", 8: "B", 25: "custom group"}


def load(fn):
    mem, meta = {}, {}
    for line in open(fn):
        if line.startswith("#"):
            continue
        p = line.split()
        if len(p) < 3 or p[2] == "ERR":
            continue
        cmd, off, data = int(p[0], 16), int(p[1], 16), bytes.fromhex(p[2])
        if cmd in (0x05, 0x0A):
            for i, b in enumerate(data):
                mem[(cmd, off + i)] = b
        else:
            meta[cmd] = data
    return mem, meta


def region(cmd, off):
    """(name, index inside the region) or (None, off)"""
    base = 0x6C0 if cmd == 0x05 else 0x000
    for g in range(3):
        b = base + 0x200 * g
        if b <= off < b + 0x180:
            return "group %d" % (g + 1), off - b
    if cmd == 0x05 and 0xCC0 <= off < 0xEC0:
        return "macro image", off - 0xCC0
    if cmd == 0x0A and 0x600 <= off < 0x800:
        return "macro image", off - 0x600
    if cmd == 0x05 and off < 0x22:
        return "record0", off
    return None, off


def main(a_fn, b_fn):
    a, _ = load(a_fn)
    b, meta = load(b_fn)
    keyids = meta.get(0x07)
    changed = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    print("%d bytes differ\n" % len(changed))
    shown, mirrored = set(), 0
    for cmd, off in changed:
        name, idx = region(cmd, off)
        if cmd == 0x0A and name in ("group 1", "group 2", "group 3", "macro image"):
            mirrored += 1
            continue                      # same bytes as cmd 05, reported there
        if name and name.startswith("group"):
            slot = idx // 3
            if (name, slot) in shown:
                continue
            shown.add((name, slot))
            def rgb(m):
                return "".join("%02X" % m.get((cmd, off - idx + slot * 3 + i), 0) for i in range(3))
            key = ""
            if keyids and slot < len(keyids) // 3:
                t = keyids[slot * 3:slot * 3 + 3]
                key = "  key-id %02x %02x %02x" % tuple(t)
            print("%-11s slot %3d (col %2d row %d)  %s -> %s%s" % (
                name, slot, slot // 8, slot % 8, rgb(a), rgb(b), key))
        elif name == "record0":
            print("%-11s byte %2d %-13s %02x -> %02x" % (
                name, idx, REC.get(idx, ""), a.get((cmd, off), 0), b.get((cmd, off), 0)))
        elif name == "macro image":
            if ("macro", 0) not in shown:
                shown.add(("macro", 0))
                n = sum(1 for k in changed if region(*k)[0] == "macro image" and k[0] == 0x05)
                print("macro image changed (%d bytes at cmd 05 0xCC0..)" % n)
        else:
            print("cmd %02x  0x%04X  %02x -> %02x" % (cmd, off, a.get((cmd, off), 0), b.get((cmd, off), 0)))
    if mirrored:
        print("\n(%d cmd 0A bytes mirror the cmd 05 changes above)" % mirrored)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
