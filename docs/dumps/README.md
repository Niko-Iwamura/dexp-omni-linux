# Raw protocol dumps

Captured from a real **320F:505B "Evision RGB Keyboard"** (128 slots) on
2026-11-24, with the read-only tools only. Keep them as fixtures: they are the
evidence behind PROTOCOL.md §5.2b and §13.

| file | what it is |
|---|---|
| `protocol-session-2026-11-24.log` | terminal transcript of the first read-only session (`scan`, `info`, `keyids`, `lighting --read`, `dump` of the group pages, `remap --layer 0..2 --read`) |
| `keyids.txt` | `omni-cli keyids` — the cmd 07 key-ID table, 128 slots × 3 bytes |
| `layer0-raw-cmd0A.txt` | `omni-cli remap --layer 0 --read` — bytes as returned by cmd 0x0A |
| `layer1-raw-cmd0A.txt` | same for layer 1 |
| `layer2-raw-cmd0A.txt` | same for layer 2 |

**Note on the `layerN-raw-cmd0A.txt` files:** the `remap` command prints its
lines as key *assignments* (`T=… X=… P=…`), which is meaningless for this data.
On 320F:505B the "layers" addressed by cmd 0x0A are the three custom-colour
groups, so each line's three bytes there are really `R G B` for that slot
(PROTOCOL.md §5.2b). Use `omni-cli group --group N --read` for a correctly
labelled dump; these raw files are kept only as the original evidence.
