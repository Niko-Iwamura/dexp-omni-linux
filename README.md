# DEXP OMNI Keyboard — Linux configurator

A Linux (Kubuntu-friendly) replacement for the Windows-only **DEXP OMNI**
keyboard configurator (`OMNI.exe`), reverse-engineered from the original
Windows binary.

* Device: DEXP Omni wired mechanical keyboard (black/white)
* Controller: Sonix **VS11K09A**, USB **`0C45:1007`**, plain USB HID — no
  kernel driver needed, everything goes through `/dev/hidraw`
* Original software: DEXP-branded **Eevision/Acrox** configurator,
  platform string `eevision_A_kb_001_20180108_ak60`

The full wire protocol is documented with binary addresses as evidence in
[PROTOCOL.md](PROTOCOL.md). The layout of the GUI is taken from the
original skin XML, so it matches the physical keyboard.

## What works (per the static RE of OMNI.exe)

* **Connection & device info** — session begin/end (cmd 01/02), device info
  (cmd 03): model id, key/slot count, macro storage capacity
* **Key remapping** — read/write the live assignment bank (cmd 07/09) and the
  stored layer pages (cmd 0A/0B), 3-byte `[T,X,P]` records: normal keys with
  modifier bitmasks, media/OEM codes, macro keys M1–M5, macro-slot bindings,
  disable
* **Lighting** — all 31 lighting modes, brightness/speed 0–4, colour,
  multi-colour flag, via the 34-byte settings record (cmd 05/06)
* **Custom per-key lighting** — 13-colour palette upload (report-ID 0xFF
  frames), per-LED palette-index table (cmd 12/1B), custom groups 0–2.
  On the retail **320F:505B** there is no palette: each custom group is a
  384-byte raw-RGB page (128 slots × 3) at `0x6C0/0x8C0/0xAC0`, also
  aliased by the cmd 0x0B pages `0x000/0x200/0x400`; mode **20 = Customize**.
  Implemented in the library (`omni_read_group` / `omni_write_group_verified`),
  in the CLI (`omni-cli group`) and in the GUI, which picks whichever path the
  attached keyboard uses and paints straight onto a keyboard diagram. Verified
  in write/restore cycles — see PROTOCOL.md §5.2b
* **System settings** — polling rate 1/2/4/8 ms, 6KRO flag, Windows-key lock
  (settings record + status query cmd 1A), W flag
* **Macros** — macro table upload in the `0x55AA` container format (cmd 15),
  capacity-checked
* **Protocol console** — send arbitrary 0x04 frames or raw 64-byte reports,
  full TX/RX log; this is the workbench for verifying the remaining
  unknowns on real hardware

## Lighting profiles (CLI)

```
omni-cli modes                              # lighting modes with the keyboard's own IDs
omni-cli lighting                           # show what the keyboard is doing now
omni-cli lighting --mode 3 --brightness 2 --color FF6600 --dry-run
omni-cli lighting --mode 3 --brightness 2 --color FF6600
omni-cli profile save night                 # store the current lighting as a file
omni-cli profile new work --mode 3 --color FFFFFF --brightness 4
omni-cli profile list | show NAME | delete NAME
omni-cli profile apply night [--dry-run]
omni-cli backup | restore FILE [--dry-run]
```

Profiles are plain text files in `~/.config/omni-linux/profiles/`. Applying
one changes only mode, brightness, speed, direction, multicolour flag and RGB
of the active settings record; polling rate, Win-lock, 6KRO and reserved bytes
stay as the keyboard has them. Mode IDs come from the Windows app's own table (see PROTOCOL.md 4.1).
Modes the app hides, and 29/30 (Audio wave, Shadow, fed by the Windows app),
are refused unless `--force` is given.

Every write first saves the current record and the first 256 bytes of settings
NVRAM to `~/.config/omni-linux/backups/`, then reads the record back and
compares every field ("written, verified by read-back"). If the keyboard
ignores or alters a write you get an error and `omni-cli restore FILE` puts
the old record back.

**First write on a new keyboard:** run `omni-cli backup`, then change
something harmless and visible (`omni-cli lighting --brightness 3`), check the
keyboard really dimmed, then set it back. The write path (cmd 01, 06, 02) has
been checked against a simulated device, not yet against a 320F:505B.

## Tested hardware

Verified on **USB 320F:505B "Evision RGB Keyboard"** (the DEXP OMNI as sold
today): framing, cmd 03/05/07/1B/0A, and per-key colour in a verified
write/restore cycle. Custom per-key lighting **does** work on this ID, but
differently than the palette path: no 0xFF palette report, no cmd 0x12 index
table — instead a raw-RGB page per custom group (see PROTOCOL.md §5.2b and
§13). `omni_supports_custom_light()` still returns 0 for this ID because the
CLI/GUI palette flow is the 0C45:1007 one; the raw-RGB path is
`perkey_write.py` for now. Take a read-only snapshot before any write
(`omni-cli dump`).

## What is still unverified on hardware

Everything above was derived **statically** from `OMNI.exe` (disassembly of
all 16 command builders, the JSON config parser and the skin). The wire
formats are cross-verified against the shipped `DefaultData/Keyboard.json`.
Not yet confirmed against a physical keyboard (no device was attached during
development):

1. interrupt-OUT delivery of the 17-byte `FF 22/44 …` palette frames
   (Windows used a control SET_REPORT — Linux `write()` normally works)
2. the bitfield layout of the 4-byte macro **event** records
   (the container format is fully known; the events are edited as raw
   `u32` in this app — see PROTOCOL.md §7)
3. which remap **layer** index is active by default (try 0–3 on the Keys page)
4. the exact meaning of the direction/FX byte and of the W flag

The protocol console + log exist exactly so you can nail these down with
your keyboard attached. If you capture something, `PROTOCOL.md` has an
"open points" section waiting for your findings.

## Building on Kubuntu

```bash
sudo apt install build-essential libgtk-3-dev
make
sudo make install          # optional: /usr/local/bin/omni-gtk, omni-cli
```

`make` builds `omni-cli` (no GTK needed) and `omni-gtk` (GUI).

## Permissions (udev)

```bash
sudo cp udev/60-dexp-omni.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

Then unplug/replug the keyboard (or re-login) and run as your normal user.
Without the rule you need `sudo`.

## Using the GUI

* **Device** — scan, connect (probes each HID interface with cmd AA and
  picks the one that answers), system settings, win-lock query, probe/pulse
* **Lighting** — global effects; custom per-key colours: pick a colour, click
  keys to paint them, then Apply (verified write). The page switches itself to
  the path the connected keyboard supports: a raw-RGB page per custom group on
  320F:505B, the 13-colour palette + cmd 0x12 index table on 0C45:1007
* **Keys** — click a key, build a new assignment, then apply to the live
  bank (immediate) or write a layer page (stored profile)
* **Macros** — edit events, upload, then bind M1–M5 (or any key via type
  0x71) on the Keys page
* **Protocol console** — send arbitrary frames, watch the log, save it

## CLI examples

```bash
omni-cli scan
omni-cli info
omni-cli lighting --read
omni-cli lighting --mode 3 --brightness 4 --speed 2 --color FF6600
omni-cli set --polling 1 --win 0
omni-cli winlock
omni-cli keyids
omni-cli remap --layer 1 --read
omni-cli remap --layer 1 --set 120=M5 --set 22=Esc     # M1 -> M5, 1 -> Esc
omni-cli dump --offset 0 --len 64
omni-cli raw --frame "04 00 00 1C 00 00 00 00"

# custom-group per-key colours (320F:505B, PROTOCOL.md 5.2b)
omni-cli group --read                            # dump group 0's colour page
omni-cli group --group 1 --save g1.bin           # back it up first
omni-cli group --group 1 --set PageUp=0200B2 --set D=FF0000
omni-cli group --group 1 --set D=FFFFFF --dry-run
omni-cli group --group 1 --from g1.bin           # restore the whole page
omni-cli group --group 0 --mode                  # switch to Customize (mode 20)
```

## Source layout

```
src/omni.c          protocol library (transport, all 16 commands, helpers,
                    custom-group raw-RGB pages for 320F:505B)
src/omni.h          public API
src/omni_keys.c     static key/mode/usage tables extracted from OMNI.exe
src/omni_layout.c   key geometry generated from the original skin XML
src/omni-cli.c      command line tool
src/gui-*.c         GTK3 configurator
udev/60-dexp-omni.rules
PROTOCOL.md         full wire-protocol documentation (with RE evidence)
```

## Legal note

This project is an independent, clean-room re-implementation based on
protocol analysis of the original Windows software. It contains no code
from DEXP/Eevision. Use at your own risk; the settings writes were matched
byte-for-byte to the original app's behaviour, but always keep the
possibility of a factory reset (unplug/replug) in mind when experimenting
with the protocol console.

License: MIT.
