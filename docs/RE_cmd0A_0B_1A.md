# OMNI.exe (DEXP OMNI / Eevision) — USB HID protocol: commands 0x0A / 0x0B / 0x1A
### Key-remap read/write + status query — full RE report

Binary: `OMNI.exe` (PE32, image base 0x400000, `.text` VA 0x401000).
All addresses are VAs. Analysis: capstone + pefile (scripts: `scripts/disas.py`, `scripts/agentB_findcallers.py`, `scripts/agentB_funcs.py`).

---

## 0. Shared transport frame (confirmed, applies to all commands here)

64-byte HID feature/output report, built in a stack buffer at `[ebp-0x44]`:

| off | size | meaning |
|-----|------|---------|
| 0 | 1 | report id / magic `0x04` |
| 1 | 2 | LE16 checksum = 16-bit sum of `buf[3..63]` (loop `i=3..0x3F`) |
| 3 | 1 | **command** |
| 4 | 1 | param0 (usually: payload length) |
| 5 | 2 | LE16 param1 (usually: memory offset) |
| 7 | 1 | 0 (padding/flags) |
| 8..63 | 56 | payload |

Response (recv buffer at `[ebp-0x84]`): `recv[7]` = status (`0xFF`→err −102/0xFFFFFF9A, `0xFE`→err −103/0xFFFFFF99), `recv[8..]` = data.

Every builder is a **thiscall method on the comm object embedded at `dev->this + 0x87C`** (the device/config controller class). Relevant object fields of that comm object:

| field | meaning |
|-------|---------|
| `+0x60C` | lock/notify object A (used by cmd 0x0A, 0x1A) — `enter` = 0x4788B0, `leave/notify` = vcall `[obj+0x14]` |
| `+0x62C` | lock/notify object B (used by cmd 0x0B — separate lock from the read path!) |
| `+0x658` | byte: **max chunk size** — 0x38 (56) normal, 0x18 (24) "slow" mode (set by 0x47F470: arg 1→56, arg 2→24; the app selects 24-byte mode for device-type-index 6, i.e. a different HID product) |
| `+0x64C` | dword: transfer-mode flag (0 for 56-byte, 1 for 24-byte) |

Generic transport: `0x47BD30` — `thiscall send_recv(this, u8* req, int req_len, u8* resp, int resp_len, int* out)` (req is copied into a 0x40 local, zero-padded; requires `this->[0x14]` and `this->[0x10]` handles non-null).

Sibling builders in the same family (for context):

| builder | cmd byte | role |
|---------|----------|------|
| 0x47CA40 | 0x01 | ping/status, zeroed params |
| 0x47CC50 | 0x02 | session end / commit (arg1=1 usually) |
| 0x47D080 | 0x03 | **get device info** (fills `this+0x1534..` struct incl. `[0x1539]`=key count, `[0x1534]`=model id) |
| 0x47ED60 | 0x05 | chunked block **read** (device config memory) |
| 0x47EAE0 | 0x06 | chunked block **write** (device config memory) |
| 0x47DDB0 | 0x07 | chunked read of the **512-byte per-slot key-ID table** |
| **0x47E300** | **0x0A** | chunked read of the **key-remap table** ← this report |
| **0x47E590** | **0x0B** | chunked write of the **key-remap table** ← this report |
| 0x47E830 | 0x12 | chunked write (512-byte table, counterpart of 0x1B; caller 0x497A7A) |
| 0x47E050 | 0x1B | chunked read of a second 512-byte table (stored at `this+0x2910`) |
| **0x47EFF0** | **0x1A** | single-shot small read ← this report |
| 0x47F1E0 | 0xAA | all-zero params — reset/abort; used as "device alive" probe (return bool) |

---

## 1. Command 0x0A — chunked READ of key-remap table — builder 0x47E300

### Signature (thiscall, `ret 0x10` = 4 stack args)
```c
int read_remap(void* this /*ecx, = dev+0x87C*/,
               u8*   dst,        // [ebp+8]   destination buffer (app-provided, ≥ total_len)
               u32   total_len,  // [ebp+0xc] bytes to read
               u32   layer,      // [ebp+0x10] remap page index (device "layer"/profile)
               u32   stride);    // [ebp+0x14] page size in bytes (0x200 in all callers)
```

### Request construction (per chunk)
```
0x47E319  buf[0] = 0
0x47E31D  memset(&buf[1], 0, 0x3F)
0x47E335  buf[0] = 4                    ; report magic
0x47E342  buf[3] = 0x0A                 ; COMMAND
0x47E36B  buf[7] = 0
loop (pos = 0; pos < total_len; pos += chunk):
  0x47E3E0/EE  buf[4] = chunk          ; chunk = min(this->[0x658], total_len - pos)   (56 or 24)
  0x47E3F2..   off = layer*stride + pos
  0x47E410     buf[5] = off & 0xFF
  0x47E42C     buf[6] = (off >> 8) & 0xFF
  checksum: buf[1..2] = LE16(sum(buf[3..63]))
  lock:   0x4788B0(this+0x60C)
  xfer:   0x47BD30(this, buf, 0x40, recv, 0x40, &rc)
  unlock: vcall [ (this+0x60C)->vtbl + 0x14 ]
  if rc != 1        return rc
  if recv[7]==0xFF  return -102 (0xFFFFFF9A)
  if recv[7]==0xFE  return -103 (0xFFFFFF99)
  memcpy(dst + pos, &recv[8], chunk)
return 1 on success
```

**Wire request (first chunk, layer 1, len 56):** `04 <cksumLE16> 0A 38 00 02 00 <56 data bytes…>`
(`off = layer*0x200 + 0` → buf[5..6] = `00 02` for layer 1.)

### Response
`recv[7]` status; **`recv[8 .. 8+chunk-1]` = table bytes** at `layer*0x200+pos`. Loop until `total_len` read.

---

## 2. Command 0x0B — chunked WRITE of key-remap table — builder 0x47E590

### Signature (thiscall, `ret 0x10`)
```c
int write_remap(void* this /*ecx*/,
                u8* src,        // [ebp+8]   source buffer
                u32 total_len,  // [ebp+0xc] bytes to write
                u32 layer,      // [ebp+0x10]
                u32 stride);    // [ebp+0x14] = 0x200
```

### Request construction — identical skeleton, differences only:
```
0x47E5E2  buf[3] = 0x0B
chunk loop as 0x0A (chunk = min(this->[0x658], remaining))
off = layer*stride + pos → buf[5..6]
0x47E6EE  memcpy(&buf[8], src + pos, chunk)      ; payload copied BEFORE checksum
checksum over buf[3..63] → buf[1..2]
lock: 0x4788B0(this+0x62C)   ; NOTE: different lock object than 0x0A!
xfer: 0x47BD30(this, buf, 0x40, recv, 0x40, &rc); unlock vcall +0x14
recv[7] checks identical (−102 / −103)
```

**Example packets (simulated from the shipped `DefaultData/Keyboard.json`, layer 1):**
```
off 0x0200 len 56: 04 dc 07 0B 38 00 02 00 | 20 00 29 20 00 3A 20 00 3B 20 00 3C 20 00 3D 20 ...
off 0x0238 len 56: 04 83 05 0B 38 38 02 00 | 00 20 00 00 20 00 00 20 00 35 20 00 1E 20 00 1F ...
...
last chunk ends at 0x0200+378 (126 keys × 3 bytes = 378 = 0x17A); page is 512 bytes, tail unused.
```

---

## 3. Command 0x1A — single-shot small read — builder 0x47EFF0

### Signature (thiscall, `ret 0xC` = 3 args)
```c
int read_small(void* this /*ecx*/,
               u8* dst,     // [ebp+8]  out buffer
               u8 len,      // [ebp+0xc] bytes to read (≤56)
               u16 offset); // [ebp+0x10] memory offset
```

### Request
```
0x47F025  buf[0] = 4
0x47F032  buf[3] = 0x1A          ; COMMAND
0x47F068  buf[4] = (u8)len       ; from [ebp+0xc]
0x47F07A  buf[5] = offset & 0xFF ; from [ebp+0x10]
0x47F08C  buf[6] = offset >> 8
0x47F05B  buf[7] = 0
checksum → buf[1..2]
lock 0x4788B0(this+0x60C); xfer 0x47BD30; unlock vcall +0x14
recv[7]==0xFF → -102 ; ==0xFE → -103
0x47F1B9  memcpy(dst, &recv[8], len)
return 1
```

**Wire:** `04 <cksum> 1A <len> <offLO> <offHI> 00 …`

Only observed use: `read_small(this, buf6, len=6, offset=0)` — 6-byte device status area at offset 0 (see §4.5/4.6: byte 3 of the result is the Windows-lock state).

---

## 4. Callers & semantics

### 4.1 0x499AB0 — "sync key bindings from device (current layer)" — calls cmd 0x0A at 0x499B28
```c
u8 tbl[0x200] = {0};
rc = read_remap(this+0x87C, tbl, this->[0x1539]*3,   // 126*3 = 378 bytes
                this->[0x1840],                       // current layer/profile index
                0x200);                               // page stride
for (key = 0; key < key_list_count(); key++) {        // key_list = this+0x1828 (std::vector), count via 0x4490D0
    u8 slot = map_key_to_slot(key);                   // std::map at this+0x17C8, operator[] 0x4491F0
    if (slot >= 0xFF || slot*3 > 0x200) continue;
    u8* keyrec = key_list[key];                       // 0x41B5A0 → *(vector.data + key*4)
    keyrec[0] = tbl[slot*3+0];  keyrec[1] = tbl[slot*3+1];  keyrec[2] = tbl[slot*3+2];
}
// then: log "keycount,layer" (0x451680/0x4518F0/0x492210) and
// call 0x4A4420() — write the (possibly modified) table back with cmd 0x0B
```
Triggered from the big device-init function (0x495040…0x495626) at **0x495546**, but **only when `this->[0x1834] == 0x14` (model id 20)** — dispatch table at 0x49562C (`model-0xB` in the jump table). Other models take other virtual calls (`+0x3C0/+0x3B8/+0x32C/+0x338/+0x35C`).

So for model 20 the app **reads the remap table at startup, refreshes the in-app key list, and immediately rewrites the table** (normalization round-trip).

### 4.2 0x499E60 — "upload/apply key bindings to device" — calls cmd 0x0B at 0x49A0AF
Mirror of 4.1 (write direction):
```c
rc = 0x451FB0(this+0x2E08); ... 0x492210(...)             // begin/log (same prologue as 4.1's epilogue)
0x417860(this->[0x1558], layer)                            // select layer (notify)
u8 tbl[0x200] = {0};
for (key = 0; key < count; key++) {
    slot = map[key]; if (slot>=0xFF || slot*3>0x200) continue;
    tbl[slot*3+0..2] = key_list[key][0..2];                // copy current assignment
}
rc = write_remap(this+0x87C, tbl, this->[0x1539]*3, this->[0x1840], 0x200);
if (rc != 1) Sleep(200);                                   // one retry pause
```
It is a **virtual method**: its address sits in a device-class vtable at `.rdata:0x69B08C` (slot offset +0x4C from 0x69B040), which is why it has no direct E8 callers (invoked through vtable dispatch).

### 4.3 0x4A4100 — "set single key assignment (model 0x14) and commit" — calls cmd 0x0B at 0x4A4405
Guarded by `this->[0x1834] == 0x14`; arg1 = key index (`[ebp+8] < key_count`):
```c
u8* keyrec = key_list[arg1];
keyrec[0] = this->[0x1839]; keyrec[1] = this->[0x183A]; keyrec[2] = this->[0x183B];
// ^ the three "current assignment" bytes staged by the UI (see 0x49A140/0x49A1F0 which fill
//   [0x1839..0x183B] either from globals or, if arg==0, with zeros)
... 0x4178C0(this->[0x1558]) ...                          // validity check
u8 tbl[0x200] = {0};
for (key = 0; key < count; key++) {
    slot = map[key];
    if (key == 0x4B && slot == 0x6A) slot = 0x6B;          // Enter(75) → slot 107
    if (key == 0x37 && slot == 0x6B) slot = 0x6A;          // Backslash(55) → slot 106
    if (slot >= 0xFF || slot*3 > 0x200) continue;
    tbl[slot*3+0..2] = keyrec_of(key)[0..2];
}
rc = write_remap(this+0x87C, tbl, this->[0x1539]*3, this->[0x1840], 0x200);
```
Note the **Enter/Backslash slot-swap quirk** (present here and in 0x4A4420, *absent* in the read path 0x499AB0 — read and write are asymmetric for keys 75/55 on model 20; the UI asset `Skin/common/Exchange-key.png` matches this "exchange key" feature).
It is a virtual method too: vtable entry at `.rdata:0x69B0F4` (slot +0xB4).

### 4.4 0x4A4420 — "write current key bindings table" — calls cmd 0x0B at 0x4A4639
Same table build as 4.3 (incl. Enter/Backslash swap), but no per-key staging:
```c
if (this->[0x1834] == 0x14) {
   build tbl from key_list; write_remap(tbl, [0x1539]*3, [0x1840], 0x200);
   if (rc != 1) Sleep(200);
}
return 0;
```
Called at the end of the read path (0x499CFD), the layer-switch handlers (0x499DE1, 0x499E4F) and after FN-mode change (0x49A1FE).

### 4.5 0x48AEA0 — "query Windows-lock / keyboard-lock state" — calls cmd 0x1A at 0x48AEE8
```c
if (this->[0x890] == 0) return;                 // device-open flag
u8 st[6] = {0};
if (read_small(this+0x87C, st, 6, 0) != 1) return;
st[3] = (this->[0xEC8] != 0) ? 1 : 0;           // EC8 = "is keyboard" flag; byte3 = win-lock state
if (this->[0x1568]) observer_cb(0x436580)(this->[0x1568], st, 6);   // push 6-byte state to listener
if (this->[0x15F8]) SendMessageW(hwnd, 0x0D01, st[4], 0);           // notify UI (msg 0xD01, wParam = st[4])
```
Reached via tiny wrapper 0x488A30. The 6-byte block read from offset 0 is a small **status page**: byte 3 (index 3) = win-lock, byte 4 (index 4) forwarded to UI as message 0xD01.

### 4.6 0x495660 — "query lock state (with 0xAA reset preamble)" — calls cmd 0x1A at 0x4956D3
Identical body to 4.5, but first:
```c
if (this->[0x890] == 0) return;
if (0x4BAC30(this) != 0) return;      // busy check
if (!cmd_0xAA(this+0x87C)) return;    // builder 0x47F1E0: 04 cksum AA 00 00 00 00 — reset/probe
read_small(this+0x87C, st, 6, 0); ... // then exactly as 4.5
```
Reached via wrapper 0x4937A0; the readiness predicate 0x4937C0 is `this->[0x890] && this->[0xEC8] && cmd_0xAA()`.

### 4.7 0x4A4670 — "commit/polling remap write, ×4-scaled variant" — calls cmd 0x0B at 0x4A4900
Called from the device-polling loop at 0x46DE1C (right after a UI "device present" check + `vcall +0x298`):
```c
log 0x4518F0(key_count, layer); 0x492210(...)             // "commit" log
u8 tbl[0x200] = {0};
for (key = 0; key < count; key++) {
    slot = map[key]; if (slot >= 0xFF || slot*3 > 0x200) continue;
    k = key_list[key];
    tbl[slot*3+0] = (u8)(((u16)k[0] * (u16)k[3]) >> 8);    // assignment byte × default byte, take high byte
    tbl[slot*3+1] = (u8)(((u16)k[1] * (u16)k[3]) >> 8);
    tbl[slot*3+2] = (u8)(((u16)k[2] * (u16)k[3]) >> 8);
}
rc = write_remap(this+0x87C, tbl, this->[0x1539]*3, this->[0x1840], 0x200);
if (rc != 1) Sleep(200);
```
`k[3]` is the **DefaultAssignment low byte** — this path writes `assignment × default / 256` (a dimming/enabling factor). It is the periodic "keep device in sync" writer, not the user-facing save. (Also note: no Enter/Backslash swap here, and `sar` used for the shift — arithmetic.)

### 4.8 Supporting facts (where the data comes from)
* **Key records**: `key_list` = `std::vector<u8*>` at `this+0x1828`; count = 0x4490D0; element accessor 0x41B5A0 → each record is ≥6 bytes: `[0..2]` = current assignment wire bytes, `[3..5]` = default assignment wire bytes.
* **JSON import pipeline** (config files like `DefaultData/Keyboard.json`): parser 0x448A40 (and siblings 0x448C10/0x448DE0/0x448F10 for other sections) reads `KeyList[i].Assignment` / `KeyList[i].DefaultAssignment` and builds records `{u16 idx; u8 a[3]; u8 d[3];}`:
  ```
  a[0]=Assignment>>16; a[1]=(Assignment>>8)&0xFF; a[2]=Assignment&0xFF;   (same for d[])
  ```
  i.e. **the 24-bit JSON value IS the 3 wire bytes, big-endian**. Records are inserted (0x449260) into a vector at `this+0x2DCC`, then applied (0x493864 → 0x4A58F0 → 0x492690 → 0x442EA0) by *replacing* the key-list vectors (`assignment` vector ← records, `default` vector cleared). No 0x200000 arithmetic exists anywhere in the code — the constant only ever appears inside the JSON data.
* **Key→slot map**: `this+0x17C8` (key→slot) and `this+0x17C0` (slot→key) are built by 0x497570 after the init handshake 0x495040:
  1. cmd 0x01 ping; cmd 0x03 device-info → `[0x1539]` = key count (126), `[0x1534]` = model id;
  2. **cmd 0x07** chunked read of **512 bytes** → `this+0x1D10`: per-slot 3-byte device key-ID codes (slot-major);
  3. cmd 0x1B chunked read 512 bytes → `this+0x2910` (second ID/LED table);
  4. cmd 0x02 end; then 0x497570 matches each app key index (0..169) against static table `0x693080` (3 dwords/key = the expected 3-byte device ID) to fill both maps (unmatched = 0xFF).
  → **Remap slots are device matrix slots, not app key indices**; the identity assumption is only a coincidence of the default layout.
* **Layer index**: `this->[0x1840]` = current layer/profile, copied during device-info parse from `this->[0x2DA6]` (0x495485). Remap page = `layer * 0x200`.
* **FN/global-mode staging** (0x49A140): for all keys 0..125 (skipping ≥0x7E): `keyrec[3] = mode_byte` where `mode_byte = table[0x689184 + this->[0x1835]]` and the mode table at 0x689184 = `{0x00, 0x50, 0x87, 0xC3, 0xFF}` (global light/mode selector staged into the *default* byte). 0x49A0E0 stages `[0x1839..0x183B]` into all `keyrec[0..2]` (or zeros when disabled) — the "macro/FN assignment broadcast" used before a full write.

---

## 5. WIRE FORMAT of key remapping (the deliverable core)

### 5.1 Memory layout (device side)
```
remap area, addressed by cmd 0x0A/0x0B offset = layer*0x200 + pos:
  page layer L (512 bytes) at L*0x200:
    entry[slot] : 3 bytes, slot = 0 .. key_count-1 (126 → 378 bytes used, rest of page unused)
```
Separate areas seen on the same transport: cmd 0x07 → 512-byte slot-ID table; cmd 0x1B/0x12 → second 512-byte table at `this+0x2910`; cmd 0x05/0x06 → generic config memory (incl. macro table: count at offset 0, `count × 0x21`-byte macro records at offset 0x22 — read by 0x48ACF0); cmd 0x1A offset 0 → 6-byte status block (win-lock).

### 5.2 3-byte entry encoding (verified against `DefaultData/Keyboard.json`, all 126 entries decode to a standard US layout)
`entry = [T, X, P]` where `T` = type/opcode:

| T | meaning | X | P | example |
|---|---------|---|---|---------|
| `0x20` | normal HID key | modifier bitmask (0 = none) | HID usage (0 = none) | `20 00 29` = ESC; `20 00 3A` = F1; `20 02 00` = LShift; `20 01 00` = LCtrl; `20 80 00` = RGUI; `20 00 00` = **disabled** |
| `0xA0` | media/consumer action | device media code | param | `A0 1B 00`, `A0 24 00` (key 16/17, between Pause and Insert — volume/mmedia row), `A0 01 00` (key 110, next to RAlt — win-lock/media) |
| `0xD0` | special/macro action | `0xA2` | macro/special index 1..5 | `D0 A2 01..05` = keys 120–124 (M1–M5 macro keys) |

Modifier bitmask (byte X for T=0x20): `01`=LCtrl `02`=LShift `04`=LAlt `08`=LGui `10`=RCtrl `20`=RShift `40`=RAlt `80`=RGui.

JSON equivalence: `Assignment = (T<<16) | (X<<8) | P` — e.g. `0x200029` ↔ wire `20 00 29`; `0xD0A203` ↔ `D0 A2 03`. The parser (0x448AF4…) splits the JSON int with `sar 16 / sar 8 / al`, the builders send the three record bytes verbatim; **nothing else is computed**.

### 5.3 Slot assignment
Slot = `map_key_to_slot(app_key_index)` where the map is built from the device-reported 512-byte key-ID table (cmd 0x07) matched against static table 0x693080. On model 0x14, writes additionally swap slots for keys 75 (Enter→slot 107) and 55 (Backslash→slot 106) — reads do not, so a driver should replicate the swap **on write only** to stay bug-compatible, or verify against real firmware.

### 5.4 Transfer sizes
Page write = `key_count*3` (378) bytes starting at `layer*0x200`, split into chunks of `this->[0x658]` = **56 bytes** (normal) or **24 bytes** (alternate HID interface selected when device-type-index == 6 at connect, 0x463E51). Checksum covers `buf[3..63]` including the zero padding beyond the chunk payload.

---

## 6. Response handling summary (all three commands)
* return `1` — success.
* return `rc` from 0x47BD30 if ≠ 1 (transport/HID error).
* `recv[7] == 0xFF` → `-102`; `recv[7] == 0xFE` → `-103`.
* Writers (0x0B) retry once after `Sleep(200 ms)` at the caller level; the read path also has caller-level retry loops elsewhere.

## 7. Caller purpose cheat-sheet
| address | purpose | command used |
|---------|---------|--------------|
| 0x499AB0 | read remap page for current layer into key list, then write back (model 0x14 only) | 0x0A (+0x0B via 0x4A4420) |
| 0x499E60 (vtbl+0x4C @0x69B08C) | upload full key bindings ("apply/save config") | 0x0B |
| 0x4A4100 (vtbl+0xB4 @0x69B0F4) | set one key's assignment (staged bytes [0x1839..B]) + commit | 0x0B |
| 0x4A4420 | commit current bindings (after read / layer switch / FN change) | 0x0B |
| 0x4A4670 | polling-loop commit with `(assignment*default)>>8` scaled bytes | 0x0B |
| 0x48AEA0 (via 0x488A30) | poll win-lock status page | 0x1A (6 bytes @ 0) |
| 0x495660 (via 0x4937A0) | poll win-lock status, with cmd 0xAA preamble + busy check | 0xAA + 0x1A |

## 8. Suggested next actions (Linux driver port)
1. Implement `read_remap/write_remap` with the exact frame above; verify chunk size 56 works (the 24-byte mode is only needed for the second HID interface of some models).
2. Dump one real page per layer via cmd 0x0A and diff against the JSON-derived table to confirm slot mapping (especially slots 106/107 for Enter/Backslash on model 0x14).
3. Read the 512-byte cmd-0x07 table to build the key↔slot map instead of assuming identity; publish it as the stable key addressing for the driver.
4. Investigate cmd 0x1A offsets > 0 (only offset 0 is used by the app — likely more status fields exist) and cmd 0xAA semantics (reset? firmware-mode probe?).
5. Macros: follow 0x48ACF0 (cmd 0x05 at offset 0x22, records of 0x21 bytes) and the macro editor builders (`Skin/XML/CustomControlXML/MacroControl.xml`, JSON keys `Action/down/Button/Delay/Type`) — the macro *content* format is stored in config memory via cmd 0x05/0x06, not via 0x0B; only the M1–M5 key binding (`D0 A2 0n`) lives in the remap table.
