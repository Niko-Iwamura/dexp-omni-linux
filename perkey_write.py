#!/usr/bin/env python3
"""One-key colour write test for the 320F:505B (PROTOCOL.md §5.2b).

Writes a single per-key RGB triple into a custom-group colour page and
verifies it by read-back. Uses ONLY stdlib + /dev/hidraw.

Usage (run as root or with the udev rule):
    python3 perkey_write.py <group 0-2> <slot 0-127> <RRGGBB> [--via 06|0B] [--dry] [--node=/dev/hidrawN]

    --via 06   write the page with cmd 0x06 at 0x6C0+group*0x200
               (default: cmd 0x0B at group*0x200; both address the same bytes)
    --dry      print the frames instead of touching the keyboard
    --node P   use /dev/hidrawN instead of auto-detecting the config interface

Example: paint the D key (slot 27) of group 0 red:
    python3 perkey_write.py 0 27 FF0000

The colour only shows while the settings record is in mode 20 (Customize)
with the matching CustomGroup; this script does not touch the mode byte.
"""
import sys, time, glob, os, select

CHUNK = 32
TIMEOUT = 1.0          # seconds to wait for an answer before giving up


def frame(cmd, length=0, off=0, data=b""):
    f = bytearray(64)
    f[0] = 0x04                      # report ID
    f[3] = cmd
    f[4] = length
    f[5] = off & 0xFF
    f[6] = off >> 8
    f[8:8 + len(data)] = data
    s = sum(f[3:64])                 # checksum over bytes 3..63
    f[1] = s & 0xFF
    f[2] = s >> 8
    return bytes(f)


class Dev:
    def __init__(self, path, dry):
        self.dry = dry
        self.f = None if dry else open(path, "r+b", buffering=0)

    def tx(self, cmd, length=0, off=0, data=b"", expect_reply=True):
        f = frame(cmd, length, off, data)
        if self.dry:
            print("  TX " + f.hex(" "))
            return bytes(64)
        self.f.write(f)
        if not expect_reply:
            return b""
        deadline = time.monotonic() + TIMEOUT
        while True:                # skip unrelated input reports
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("cmd %02X: no answer in %.1fs" % (cmd, TIMEOUT))
            if not select.select([self.f], [], [], left)[0]:
                raise TimeoutError("cmd %02X: no answer in %.1fs" % (cmd, TIMEOUT))
            r = self.f.read(64)
            if len(r) == 64 and r[3] == cmd:
                return r

    def read_page(self, base, total=384):
        out = bytearray()
        off = 0
        while off < total:
            n = min(CHUNK, total - off)
            r = self.tx(0x0A, n, base + off)
            out += r[8:8 + n]
            off += n
        return bytes(out)

    def write_page(self, cmd, base, data):
        off = 0
        while off < len(data):
            n = min(CHUNK, len(data) - off)
            self.tx(cmd, n, base + off, data[off:off + n])
            off += n


VID_PIDS = ("320F:505B", "0C45:1007")


def config_nodes():
    """HID nodes whose report descriptor has the vendor collection the
    config protocol uses: usage page 0xFF1C with report ID 4 (PROTOCOL.md 13).
    Probing every /dev/hidraw* blindly hangs on the boot-keyboard interface
    (no report IDs) and on unrelated devices such as the touchpad."""
    out = []
    for desc in sorted(glob.glob("/sys/class/hidraw/hidraw*/device/report_descriptor")):
        node = "/dev/" + desc.split("/")[4]
        try:
            with open(desc, "rb") as fh:
                d = fh.read()
        except OSError:
            continue
        if b"\x06\x1c\xff" in d and b"\x85\x04" in d:
            out.append(node)
    if not out:
        sys.exit("no HID interface exposes report ID 4 / usage page 0xFF1C\n"
                 "(is the keyboard plugged in?)")
    return out


def open_dev(dry, node=None):
    if dry:
        return Dev(None, True), None
    cands = [node] if node else config_nodes()
    tried = []
    for path in cands:
        try:
            d = Dev(path, False)
            d.tx(0x03)             # device info: only the right node answers
            return d, path
        except Exception as e:     # PermissionError, TimeoutError, OSError
            tried.append("%s: %s" % (path, e))
            continue
    sys.exit("no candidate node answered cmd 0x03:\n  " + "\n  ".join(tried))


def main(argv):
    # --via takes either "--via 06" or "--via=06"; everything else is positional
    args = argv[1:]
    pos, node, via, dry = [], None, 0x0B, False
    i = 0
    while i < len(args):
        x = args[i]
        if x == "--via":
            i += 1
            if i >= len(args):
                sys.exit("--via needs a value (06 or 0B)")
            v = args[i]
        elif x.startswith("--via="):
            v = x.split("=", 1)[1]
        else:
            v = None
        if v is not None:
            v = v.strip().lower()
            if v in ("06", "6", "0x06"):
                via = 0x06
            elif v in ("0b", "0x0b", "11", "0x11") or v == "11":
                via = 0x0B
            else:
                sys.exit("--via must be 06 or 0B, got %r" % v)
        elif x.startswith("--node="):
            node = x.split("=", 1)[1]
        elif x == "--dry":
            dry = True
        elif x.startswith("--"):
            sys.exit("unknown flag: " + x)
        else:
            pos.append(x)
        i += 1
    if len(pos) != 3:
        sys.exit(__doc__)
    group, slot, rgb = int(pos[0]), int(pos[1]), bytes.fromhex(pos[2])
    if not 0 <= group <= 2 or not 0 <= slot <= 127 or len(rgb) != 3:
        sys.exit("bad args")
    if via not in (0x06, 0x0B):
        sys.exit("--via must be 06 or 0B")

    d, path = open_dev(dry, node)
    print("device: %s   target: group %d slot %d -> %s" %
          (path or "(dry-run)", group, slot, rgb.hex().upper()))

    d.tx(0x01)                              # begin session
    page = bytearray(d.read_page(group * 0x200))
    old = page[slot * 3:slot * 3 + 3]
    page[slot * 3:slot * 3 + 3] = rgb
    base = (0x6C0 + group * 0x200) if via == 0x06 else group * 0x200
    print("write %d bytes via cmd %02X @ 0x%03X:" % (len(page), via, base))
    d.write_page(via, base, bytes(page))
    time.sleep(0.05)
    d.tx(0x02)                              # commit

    back = d.read_page(group * 0x200)
    ok = back[slot * 3:slot * 3 + 3] == rgb
    print("slot %d was %s, now %s  ->  %s" %
          (slot, old.hex().upper(), back[slot * 3:slot * 3 + 3].hex().upper(),
           "WRITE VERIFIED" if ok else "WRITE DID NOT STICK"))


if __name__ == "__main__":
    main(sys.argv)
