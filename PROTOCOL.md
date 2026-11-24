# DEXP OMNI Keyboard — USB HID Protocol (reverse-engineered)

Reverse-engineering target: `OMNI.exe` from the DEXP Omni driver package
("dexp-omni-white driver", DEXP-branded build of the **Eevision / Acrox** configurator,
platform string `eevision_A_kb_001_20180108_ak60`).

* Keyboard MCU: **VS11K09A** (rebranded Sonix keyboard controller)
* USB VID/PID: **`0C45:1007`** (Sonix Technology)
* Transport: plain **USB HID** (no WinUSB/kernel driver needed) — output reports
  written to a vendor HID interface, input reports read back from it.
* Everything below was extracted statically from the Windows binary (i386, Dec 2023
  build); function addresses of the original exe are given as evidence.

---

## 1. Transport layer

The Windows app opens the device's HID interfaces with `CreateFileW` and exchanges
**64-byte reports** through `WriteFile`/`ReadFile` (async, 1000–5000 ms timeouts).

```
request  (64 bytes, written to the device):
  byte 0    0x04            report ID / command class ("magic")
  byte 1    checksum low    u16-le checksum of bytes 3..63
  byte 2    checksum high
  byte 3    command         see §3
  byte 4    chunk length    payload bytes in this packet (0..56)
  byte 5    offset low      u16-le offset (meaning depends on command)
  byte 6    offset high
  byte 7    0x00            always zero
  byte 8..63 payload (56 bytes max)

response (64 bytes, read back from the device):
  byte 7    status: 0xFF -> error (-102), 0xFE -> error (-103), anything else: OK
  byte 8..  response payload
```

* checksum = `sum(bytes[3..63])` as a **16-bit** value, stored little-endian at bytes 1–2.
* Builder evidence: `0x47CA40` (cmd 01) … `0x47F1E0` (cmd AA); all call the shared
  send/recv wrapper `0x47BD30` which pads the buffer to 64 bytes, logs the full
  frame (`"%02x %02x …"` at `0x68F5D0`) and performs write + read.
* **Chunk size** is normally `0x38` (56 = 64 − 8 header). Some devices use a second
  interface with 32-byte reports (chunk `0x18`); selected by mode function `0x47F470`:
  `dev+0x64C` = interface flag, `dev+0x658` = chunk size. The keyboard uses 56.
* On **Linux** the interface is a `hidraw` node; `write()` the 64-byte frame
  (report ID `0x04` included) and `read()` the 64-byte answer.

### 1.1 Small side-channel reports (lighting palette)

Besides the 04-class frames, the app sends **17-byte output reports with report ID
`0xFF`** via `HidD_SetOutputReport` (builder `0x48B260`) to upload the 13-colour
lighting palette:

```
byte 0     0xFF     report ID
byte 1     0x22 or 0x44     command (sent twice, once with each value)
byte 2     0xAA/0xAB/0xAC   palette component 0/1/2 first half (R/G/B, 7 bytes)
           0xAD/0xAE/0xAF   palette component 0/1/2 second half (6 bytes)
byte 3     0x00
byte 4..   payload (7 bytes for AA/AB/AC, 6 bytes for AD/AE/AF)
```

The payload is 13 scaled colour bytes split as 7+6. Each byte is computed as
`(base_colour * brightness_scale) >> 8`, capped at `0xF7` (the scale is per-palette
entry, default 0xFF). The app sends 12 frames in this order:

```
FF 22 AA r[0..6]   FF 22 AD r[7..12]
FF 44 AA r[0..6]   FF 44 AD r[7..12]
FF 22 AB g[0..6]   FF 22 AE g[7..12]
FF 44 AB g[0..6]   FF 44 AE g[7..12]
FF 22 AC b[0..6]   FF 22 AF b[7..12]
FF 44 AC b[0..6]   FF 44 AF b[7..12]
```

---

## 2. Session and device-info

```
1) cmd 01  "begin session"
2) ... transfers ...
3) cmd 02  "commit / end session"
```

Read current state on connect (function `0x495040`):

```
cmd 03   -> 34-byte device info block
           info[5] = key/slot count      (e.g. 126)
           info[6] = macro storage capacity in 128-byte units
cmd 07   -> key-ID table, keycount*3 bytes (see §6)
cmd 1B   -> per-LED palette-index table, keycount bytes (see §5.3)
cmd 05 offset 0, len 1   -> active settings-slot index  P
cmd 05 offset P*0x21, len 0x22 -> 34-byte settings record (see §4)
```

`cmd 03` is a fixed 34-byte read (builder `0x47D080`, chunked like cmd 05 with
total = 0x22). `cmd AA` is a parameterless probe/pulse used to check the device is
alive (`recv[7]`/`recv[8]` status).

---

## 3. Command reference (all builders verified)

| cmd  | builder    | R/W  | meaning |
|------|------------|------|---------|
| 0x01 | `0x47CA40` | —    | begin session |
| 0x02 | `0x47CC50` | —    | commit / end session (Sleep(10) before send) |
| 0x03 | `0x47D080` | read | read 34-byte device info (key count, macro capacity) |
| 0x05 | `0x47ED60` | read | read block from settings NVRAM at u16 offset (chunked, data from `recv[8]`) |
| 0x06 | `0x47EAE0` | write| write block to settings NVRAM at u16 offset (data at `buf[8]`) |
| 0x07 | `0x47DDB0` | read | read **key-ID table** (length = keycount×3, set beforehand) |
| 0x09 | `0x47D360` | write| write **live key-assignment table** (bank 0, 3 bytes/slot) |
| 0x0A | `0x47E300` | read | read **key-remap page** `layer*0x200 + off` |
| 0x0B | `0x47E590` | write| write **key-remap page** `layer*0x200 + off` |
| 0x12 | `0x47E830` | write| write **per-LED palette-index table** (1 byte/LED) |
| 0x15 | `0x47D8E0` | write| upload **macro definition table** (0x55AA image, own address space) |
| 0x17 | `0x47DB70` | write| upload "ShootData" rapid-fire table (mouse only, fixed 56-byte chunks, no lock) |
| 0x1A | `0x47EFF0` | read | small read: `buf[4]=len`, `buf[5..6]=offset` (win-lock status etc.) |
| 0x1B | `0x47E050` | read | read **per-LED palette-index table** (length = keycount) |
| 0x1C | `0x47CE70` | —    | parameterless keyboard control pulse (`04 1C 00 1C 00…`) |
| 0x27 | `0x47D620` | write| write **second key-assignment bank** (grouped-apply flow) |
| 0xAA | `0x47F1E0` | —    | parameterless probe/reset pulse |

Common errors: `recv[7]==0xFF → -102`, `recv[7]==0xFE → -103`.

---

## 4. Settings record (34 bytes, the "profile")

Stored in the settings NVRAM addressed by cmd 05/06. Read with
`cmd 05 (offset P*0x21, len 0x22)` where `P` = byte read from offset 0
(builder of the read path: `0x48ACF0`; write path: vtable slot 170 `0x497340`
writes the modified record back to `arg*0x21` inside a 01…02 session).

Layout (cross-verified against the `LightInfo`/`SystemStages` JSON readers
`0x446F50` / `0x4514D0` and the record writer `0x497340`):

| off | size | field |
|-----|------|-------|
| 0   | 1 | slot/profile index byte (written back verbatim) |
| 1   | 1 | **lighting mode** (`SelectItem`). Modes 0x1D/0x1E (app-driven audio effects) are remapped to **0xFE** on the wire |
| 2   | 1 | **brightness** 0–4 |
| 3   | 1 | **speed** 0–4 |
| 4   | 1 | **direction / FX** (`Fx`) |
| 5   | 1 | **multi-colour flag** (`MultiColor`, bool) |
| 6–7 | 2 | reserved (pass through from device) |
| 8   | 1 | **Blue** |
| 9   | 1 | **Green** |
| 10  | 1 | **Red**  ← note the wire order is B, G, R |
| 11  | 1 | reserved |
| 12  | 1 | `ZZCCLedMode` ("Surmount" effect variant) |
| 13  | 1 | `WFlag` |
| 14  | 1 | `Key6Flag` (NKRO / 6KRO) |
| 15  | 1 | `WinFlag` (Windows-key lock) |
| 16  | 1 | `PollingRate` (1/2/4/8 ms ⇒ 1000/500/250/125 Hz; see table `0x688D88`) |
| 17–24| 8 | reserved (byte 0x10 is zeroed by the app) |
| 25  | 1 | `CustomGroup` — active custom-light group 0–2 |
| 26–33| 8 | reserved |

A second, separate writer (`0x48AFA0`) stores 21-byte snapshots at `slot*0x64`
(saved profile slots) — the app uses it for its own profile bookkeeping; the
read path for those slots is `cmd 05 (offset idx*0x21, len 0x22)`.

**Colour bytes differ by device (verified on 320F:505B).** After setting
Corrugated with RGB 255,0,0 in the Windows app the record changed from
`00 12 04 04 00 00 EE 00 F1 08 00 …` to `00 01 04 04 00 00 FF 00 00 08 00 …`:
on this keyboard the colour is **R,G,B at bytes 6, 7, 8**; byte 9 (0x08 in both
reads) and byte 10 are not colour and must be left alone. The B,G,R-at-8..10
layout in the table above is the template/0C45:1007 reading and is not valid
for 320F:505B. (Earlier reads of "0008F1" as RGB were therefore wrong; the old
colour was R=EE G=00 B=F1.)

### 4.1 Lighting modes (mode byte of the record)

**Source of truth: `DefaultData/default_light.json`** of the Windows app. It
holds four lists (one per device type); each entry has a `LightName` (the
text-resource id) and a numeric `value`, which is the mode byte of the record,
plus a `visible` flag. The IDs are **not** the order of the strings in
`Skin/lan_*.xml` (earlier revisions of this document assumed that and were
wrong). List 0 (31 entries) is the one whose visible entries match the
effect dropdown shown for a 320F:505B keyboard (checked against screenshots,
and the keyboard's current mode 18 shows as "Speed" in the dropdown header).

```
 id  name                          id  name
  1  Corrugated                    12  Roll
  2  Cloud                         13  Wave Bar
  3  Serpentine                    14  Cartoon
  4  Spectrum                      15  Rain
  5  Breath & Gradual Changing     16  Scan
  6  Light On Constantly           17  Surmount (ZZCC variant, byte 12)
  7  Keypress Reaction             18  Speed
  8  Ripples                       20  Customize (per-key; custom group byte 25)
  9  Traverse                      29  Audio wave  (fed by the app; 0x1D -> 0xFE on the wire)
 10  Stars                         30  Shadow      (hidden, fed by the app; 0x1E -> 0xFE)
 11  Blowing Fireworks
```

Hidden entries (not offered by the app for this keyboard): 0 Corrugated, 19 / 25 /
28 Surmount, 21 Keypress Reaction, 22 Four-colour Breath, 23 / 26 Four-colour
Flash, 24 / 27 Light Off. Light Off is therefore not a mode here; brightness 0
is the way to switch the lights off. Lists 1-3 (other device types) use small
numbering 0..8 and include Pulse, RGB Flash Quickly, Emanate and Light Off.

Brightness and speed are 0–4 (sliders `deng_light_slider`, `deng_speed_slider`,
`min=0 max=4` in `LightControl.xml`).

---

## 5. Lighting

### 5.1 Global effects

Set mode / brightness / speed / colour by editing the settings record (§4) and
writing it back inside a 01…02 session. The default config
(`DefaultData/Keyboard.json`) shows the JSON mirror: `SelectItem`, `Light`,
`Speed`, `MultiColor`, `Red/Green/Blue`, `ZZCCLedMode`, `OldLedmode`.

### 5.2 Custom mode (per-key lighting)

**On 0C45:1007 (the template-derived path)** custom per-key lighting uses
**palette indices**, not raw RGB:

1. Upload the 13-colour palette with the `FF 22/44 AA…AF` reports (§1.1).
2. Upload the per-LED palette-index table with **cmd 0x12** — one byte per LED,
   value 0–12 = palette colour index (255 ≈ off/unset). Read the current table
   with **cmd 1B** (length = key count).
3. Set the settings record to mode 17 (Customize) + `CustomGroup` 0–2 and commit.

The app keeps three custom groups (`light_combo_custommodeSelect`, userdata 0–2).

### 5.2b Custom mode on 320F:505B — raw RGB, no palette (verified in write)

Confirmed on hardware (Nov 2026), both from a read-only dump and from a
verified write/restore cycle:

* Custom per-key colour is a **raw RGB triple per slot**, not a palette index.
  The three custom groups are **384-byte pages** (128 slots × 3 bytes, slot
  order `col*8 + row`, same as the cmd 07 key-ID table) living in the
  settings-NVRAM address space at

  ```
  group 0/1/2  ->  0x6C0 / 0x8C0 / 0xAC0
  ```

  and reachable from **two** independent address spaces:

  | write | address used | alias |
  |---|---|---|
  | cmd **0x06** (settings NVRAM block) | `0x6C0 + group*0x200` | — |
  | cmd **0x0B** (remap-page write)     | `group * 0x200`         | cmd 0x0A read at `0x000/0x200/0x400` returns the *same bytes* |

  Byte-for-byte cross-check of all three pages through cmd 0x05 @0x6C0/0x8C0/
  0xAC0 vs cmd 0x0A @0x000/0x200/0x400: **0 mismatches** (23 / 20 / 37 lit slots).

* There is **no 0xFF report ID** on this keyboard (§13), so the §1.1 palette
  frames and the cmd 0x12 index table do not apply. Consequently `cmd 1B`
  returns the identity map `00 01 … 7F` (an LED-per-slot map), *not* a
  palette-index table — do **not** write it with cmd 0x12.

* Per-key colour writing **is supported** on this ID (the README's "disabled"
  note is wrong): write the 384-byte page inside a `cmd 01 … cmd 02` session,
  then read it back. Mode byte of the settings record must be **20 = Customize**
  (`omni_mode_names[20]`), with `CustomGroup` 0–2 selecting which page the
  keyboard lights.

* Implemented as `omni_read_group()`, `omni_write_group()`,
  `omni_write_group_verified()`, `omni_group_set_slot()` and
  `omni_group_print()` in `src/omni.c`; `omni-cli group` exposes them
  (`--read`, `--save FILE`, `--from FILE`, `--set KEY=RRGGBB`, `--mode`).
  The GUI builds both custom panels and shows the one matching the connected
  keyboard (`app_set_custom_path()`), because pages are built before the device
  is known; painting goes straight onto the keyboard diagram.

* Verified sequence (`perkey_write.py` and `omni-cli group`): read the page →
  patch the
  3 bytes → write via cmd 0x0B *or* cmd 0x06 → commit → read back. Painting
  D (slot 27) of group 0 `FFFFFF → FF0000` and back verified on both paths;
  the full page returned byte-identical afterwards. A proof of concept: the
  retail unit had a hand-painted group 0 (red Esc, coloured F-row, white
  WASD/arrows, `02 00 B2` PageUp) matching the Windows app's "Customize" /
  Group 1 screen.

### 5.3 Palette colour data

13 entries × {R,G,B,scale}. The wire payload is the *scaled* value:
`(colour * scale) >> 8`, clamped to `0xF7` (code at `0x48B3A6-0x48B4B6`).

---

## 6. Key identification and remapping

### 6.1 Key-ID table (cmd 07)

The device reports a table of `[T,X,P]` triples, one per slot
(`keycount × 3` bytes), identifying which key lives in which slot.
The app matches it against a static 171-entry reference table at `0x693080`
(records of 12 bytes; the match triple sits at offsets +0/+4/+8, builder
`0x497570`) to build key↔slot maps:

```
normal key        T=0x20, X=0x00,        P=HID usage          (e.g. ESC = 20 00 29)
modifier key      T=0x20, X=bitmask,     P=0x00               (LCtrl=01 LShift=02 LAlt=04
                                                             LWin=08 RCtrl=10 RShift=20
                                                             RAlt=40 RWin=80)
media/OEM key     T=0xA0, X=code,        P=0x00               (0x1B, 0x24, 0x01 seen)
macro key M1–M5   T=0xD0, X=0xA2,        P=0x01..0x05
```

### 6.2 Remap pages (cmd 0A / cmd 0B)

Each remap *layer* is a 512-byte page at device offset `layer * 0x200`
(layer index comes from app field `+0x1840`). 126 slots × 3 bytes = 378 bytes
used. The 3-byte entry is the assignment:

```
20 <mods> <usage>   normal key (mods bitmask as above; usage 0 = disabled)
A0 <code> 00        media key
D0 A2 01..05        macro M1–M5
71 <macro#> …       key bound to macro slot (used by the live bank, cmd 09)
```

The app's JSON `KeyList[i].Assignment` is simply `(T<<16)|(X<<8)|P`
(e.g. ESC = 0x200029). **Slot ≠ key index** — translate through the key-ID
table. Two write paths exist: cmd 0x0B writes layer pages, cmd 0x09 writes the
live bank that mirrors back through cmd 07.

### 6.3 Win-lock status

`cmd 1A (len=6, offset=0)` returns a status block; byte 3 of the payload
(`recv[11]`) = Windows-key lock state (pollers `0x48AEA0`/`0x495660`).

---

## 7. Macros (cmd 15)

The macro table is a self-describing image uploaded to its own address space
(command 0x15, offsets restart at 0 within the table):

```
+0x00  u16  0x55AA          magic
+0x02  u16  total_len       total table length in bytes
+0x04  u16  entry_count N   number of macros
+0x06  10 B  reserved (uninitialized in the Windows app — zero it)
+0x10  u16  offset[N]       absolute offset of each entry's record block
      per entry:  u16 event_count | 2 B reserved | event_count × u32 events
```

Capacity = `device_info[6] << 7` bytes; the app refuses bigger tables.
The 4-byte event records are produced by the UI text parser
(`0x4BCAD0`→`0x44ECD0`) from `macro.config` / `.mac` files — key-code and
delay bitfields still need a hardware capture to document exactly.

---

## 8. Init / apply sequences observed in the app

**Connect (read state)** — `0x495040`:
```
cmd 01
cmd 03 (device info)
cmd 07 (key-ID table, len = keycount*3, set via 0x47F440)
cmd 1B (palette-index table)
cmd 02
cmd 05 (offset 0, len 1)   -> P
cmd 05 (offset P*0x21, len 0x22) -> settings record
```

**Apply lighting** — vtable slot 170 (`0x497340`):
```
cmd 01
cmd 06 (settings record, len 0x22, offset P*0x21)
cmd 02
```

**Apply key bindings** — `0x495B60` (bank 0 via cmd 09), `0x4961A0` (bank via
cmd 27), remap pages via cmd 0x0B — always inside `cmd 01 … cmd 02`.

---

## 9. Device-side memory map (settings NVRAM, as addressed by cmd 05/06)

```
0x0000      1 byte   active settings-slot index P
idx*0x21    34 B     settings record (see §4); slot 0 starts right after the
                     index byte, consecutive records overlap by one byte —
                     replicate the app's exact offsets
layer*0x200 512 B    remap pages (cmd 0A/0B use this addressing)
```

The key-ID table (cmd 07), per-LED table (cmd 1B/0x12), macro table (cmd 15)
and second assignment bank (cmd 27) live in **separate** banks selected by the
command byte — their offsets are positions within their own bank.

---

## 10. Linux implementation notes

* Enumerate `hidraw` nodes whose `HID_ID` is `0003:00000C45:00001007`; probe
  each node with `cmd AA` (a 64-byte `04 …` frame) — the node that answers
  with `recv[7] ∉ {0xFF,0xFE}` is the config interface.
* `write()` the 64-byte frame directly (report ID 0x04); `read()` the 64-byte
  response. Use `poll()` with ~1 s timeout. Drain stale input before requests.
* The `FF 22/44 …` palette frames are 17-byte reports with report ID 0xFF —
  `write()` them on the same node (the Windows app used `HidD_SetOutputReport`,
  i.e. a control SET_REPORT; interrupt-OUT delivery is normally accepted too,
  verify on hardware).
* Chunk size 56 bytes; always wrap transfers in `cmd 01 … cmd 02`.
* Zero all reserved/uninitialized bytes (the Windows app leaks heap garbage
  into some of them — do not copy that behaviour).

## 11. Open points — to verify on hardware

Everything in this document was derived **statically** from `OMNI.exe`
(disassembly of all 16 command builders, the JSON config parser, and the
UI skin). The frame formats are cross-verified against the shipped
`DefaultData/Keyboard.json`, but the following have not been observed on a
physical keyboard yet. The Linux app (`omni-gtk`, Protocol console tab,
`omni-cli raw`) is the workbench for closing them:

1. ~~**Palette transport**~~ — **resolved for 320F:505B: N/A.** That unit has
   no 0xFF report ID and no palette at all; per-key colour is raw RGB, see
   §5.2b. Still open for a genuine 0C45:1007 unit if one surfaces: the
   `FF 22/44 …` frames went out via `HidD_SetOutputReport` (control
   SET_REPORT) on Windows, while Linux `write()` on hidraw uses
   interrupt-OUT.
2. **Macro event records** — the container layout of cmd 15 is fully
   known, but the 4-byte event bitfields (key / delay encoding) come from
   the UI text parser (0x4BCAD0 → 0x44ECD0) and a stored `.mac` file.
   Decode by writing a macro in the Windows app, dumping the table via
   cmd 05, and correlating.
3. ~~**Default remap layer index**~~ — **resolved for 320F:505B: layers 0–2
   are not remap layers at all** but the three custom-colour groups
   (§5.2b); there is no live-bank mirror to compare against. Still open for
   0C45:1007.
4. **Direction/FX byte and W flag** — settings record bytes 4 and 13 are
   passed through from the UI (`Fx`, `WFlag`) without obvious consumers;
   observe LED behaviour per mode while sweeping the value.
5. **cmd 0x27 bank address** — see RE_cmd09_27_1C_15_17.md §2.4.

---

## 12. File evidence map

| address | function |
|---------|----------|
| `0x47BD30` | send 64-byte request + read 64-byte response |
| `0x47F440` | set expected transfer length (≤ 0x200) |
| `0x47F470` | interface mode (56 vs 24-byte chunks) |
| `0x446F50` | read `LightInfo` JSON → struct (mode/brightness/speed/fx/multicolor/R/G/B/zzcc) |
| `0x4514D0` | read `SystemStages` JSON → struct (repeat/delay/key6/polling/wflag/winflag) |
| `0x4471C0` | write `LightInfo` back |
| `0x48B260` | 12× palette upload (FF 22/44 AA…AF) |
| `0x495040` | connect / read full state |
| `0x497340` | apply settings record (vtable slot 170 of `CEevisionKeyboardDevice`, vtable `0x69AD20`) |
| `0x48ACF0` | load settings record from device |
| `0x497570` | build key↔slot maps from cmd-07 table vs static table `0x693080` |
| `0x688D88` | per-device polling-rate value table (1/2/4/6/8 ms) |
| `0x693080` | 171×12-byte key reference table (match triple at +0/+4/+8) |


---

## 13. Hardware findings — USB 320F:505B "Evision RGB Keyboard"

Read-only session on a real keyboard (Nov 2026): cmd 03, 05, 07, 1B, 1A, 0A
only; nothing was written. Wire format of §1 holds; several semantics differ.

**Interfaces.** if0 = boot keyboard (no report IDs). if1 = NKRO keyboard
(ID 1), system/consumer control, mouse (ID 5) and a vendor collection (usage
page 0xFF1C) with **report ID 4, 63 data bytes in and out** = the 64-byte
frames of §1. There is **no 0xFF report ID**, so the 17-byte palette frames
of §1.1 do not exist on this keyboard. Tools should skip interfaces whose
descriptor lacks report ID 4.

**Reply echo.** The answer repeats bytes 1-7 of the request (checksum, cmd,
len, offset) — also for error replies (seen with cmd 1A). So `resp[3] == cmd`
can be checked. Unrelated input reports (other report IDs, e.g. a 3-byte
consumer report) can arrive between request and answer and must be skipped.

**cmd 03.** `AA 55 00 00 0D 80 50 00 02 …`: [0..1] = AA 55, [4] = 0x0D,
[5] = 0x80 → **128 slots**, [6] = 0x50 → macro capacity 0x50<<7 = 10240 B.
Other bytes unknown.

**Settings record (cmd 05, slot 0).** `00 12 04 04 00 00 EE 00 F1 08 00 …`:
mode 0x12, brightness 4, speed 4, B/G/R = F1/08/00 at [8..10]. Byte [6] = 0xEE
(documented as reserved). Polling [16] reads 0, not 1/2/4/8.

**cmd 07 key-ID table.** 128 triples laid out as a **16-column x 8-row
matrix, slot = col*8 + row**. Row 7 of columns 0-7 holds **M1-M8**
(`D0 A2 01..08`), not M1-M5. Slots 71 and 79 are unused (`20 00 00`).
Besides T=20/A0/D0 there is **T=0x30 = 16-bit consumer usage** with X = low
byte, P = high byte: `30 E2 00` Mute, `30 E9 00` Vol+, `30 EA 00` Vol-,
`30 83 01` = 0x0183 (AL Consumer Control).

**cmd 1B.** Returns the identity map `00 01 02 … 7F` with `FF` at slots 71 and
79 (the two unused ones) — an LED-per-slot map, **not** the 0-12 palette-index
table of §5.2/5.3. Do not write it with cmd 0x12.

**Per-key colour — SOLVED.** Custom per-key lighting works on this ID, just not
the way §5.2 describes: there is no palette and no index table, only a **raw RGB
triple per slot**. The three custom groups are 384-byte pages at
`0x6C0/0x8C0/0xAC0` (settings NVRAM, cmd 05/06) and are **the same bytes** as
the pages `0x000/0x200/0x400` reached with cmd 0A/0B — so "remap layers 0–2"
on this hardware are actually the UI's Group 1/2/3 colour pages. Verified in a
write test: group 0 slot 27 (D) set to `FF0000` through **both** cmd 0x0B and
cmd 0x06, read back through both address spaces, restored to `FFFFFF`, page
byte-identical. Full layout and the exact frame sequence: **§5.2b**.

**cmd 1A.** Answers status 0xFF (-102) for len 6, offset 0. Win-lock state is
available as record byte [15] instead (0 here).

**cmd 0A remap pages.** Layers 0 and 1 (0x000, 0x200) are slot-indexed
triples; `00 00 00` = no override, `FF FF FF` = a marker whose meaning is
unknown. Layer 0 has FF exactly at slots 11, 18, 19, 27 (A, W, S, D) and 109,
116, 117, 125 (Left, Up, Down, Right); layer 1 has other FF clusters. The
region at 0x600 is **not a remap page**: it contains the macro table image
(magic `AA 55` at 0x638, total_len 0x146 = 0x12 + 4 + 76*4, N = 1, one macro
of 76 events), which confirms the §7 container layout and shows the macro
area is readable through cmd 0A. Event dwords alternate `xx 00 81 01` /
`yy 00 01 01`; their meaning is still open. Layer 2 (0x400) was not read (a
stray input report interrupted the transfer).


### 13.1 Where "0C45:1007 / Sonix VS11K09A" came from (correction)

`DefaultData/Keyboard.json` in the Windows app is a *default profile
template*: its `DeviceBasicInfo` says VID 3141 (0x0C45), PID 4103 (0x1007),
chip "VS11K09A", model string "eevision_A_kb_001_20180108_ak60", and a
126-entry KeyList. Earlier revisions of this document took that template for
the hardware's USB ID. It is not: no code in the real OMNI.exe compares
against 0C45:1007.

The app's own device table (static initialiser at VA 0x402241..0x4024xx, one
8-dword record per device: `d0 d1 d2 d3 VID PID kind d7`) lists
  320F:7074 kind 2,  **320F:505B kind 1**,  320F:1004 kind 3,  0C76:1630 kind 7
and the matching code also tests 320F:50EC and 320F:221D. 320F is Evision's
own vendor ID. The retail DEXP Omni (verified on hardware) is **320F:505B**,
128 slots, which is why section 13's observations differ from the
template-derived sections. What `kind` selects in the code (palette frames,
LED-table semantics, key layout) is not yet traced; treat any section that
relied on the 0C45:1007 template (section 5 lighting in particular) as
unverified for this keyboard.

**cmd AA (probe).** On 320F:505B it is answered with status 0xFF
(`04 AA 00 AA 00 00 00 FF …`): unsupported. The tools detect the
configuration interface with cmd 03 instead.

**Mode byte of the unit tested:** 18 = "Speed" (not "Stars"; the earlier
reading of 18 as Stars came from the wrong, language-file-order table).
