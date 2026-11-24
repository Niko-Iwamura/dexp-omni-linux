/*
 * omni.h — DEXP OMNI keyboard configuration library (public API)
 *
 * Reverse-engineered from the DEXP/Eevision "OMNI.exe" Windows configurator.
 * Transport: USB HID 0C45:1007 or 320F:505B, 64-byte reports with 0x04 class byte.
 * See PROTOCOL.md for the full wire documentation.
 */
#ifndef OMNI_H
#define OMNI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OMNI_VID 0x0C45u
#define OMNI_PID 0x1007u
/* second known ID: "Evision RGB Keyboard" (verified read-only on hardware,
 * see PROTOCOL.md section 13) */
#define OMNI_VID2 0x320Fu
#define OMNI_PID2 0x505Bu

#define OMNI_REPORT_SIZE   64
#define OMNI_CHUNK_SIZE    56
#define OMNI_KEYREF_COUNT  171
#define OMNI_PALETTE_COUNT 13
#define OMNI_SETTINGS_LEN  34   /* 0x22 */
#define OMNI_MAX_KEYS      171
#define OMNI_REMAP_PAGE    0x200

/* error codes */
#define OMNI_OK            0
#define OMNI_ERR          -1     /* generic / IO error */
#define OMNI_ERR_NODATA   -2     /* timeout / no response */
#define OMNI_ERR_STATUS   -102   /* device answered 0xFF in recv[7] */
#define OMNI_ERR_STATUS2  -103   /* device answered 0xFE in recv[7] */

/* ------------------------------------------------------------------ */
/* Device handle                                                       */

typedef struct omni_dev omni_dev;

struct omni_device_info {
    uint16_t vid, pid;
    char   hidraw_node[64];   /* e.g. /dev/hidraw2 */
    char   hid_name[128];     /* USB product string */
    uint8_t raw[34];          /* 34-byte info block from cmd 03 */
};

/* Scan for OMNI keyboards. Returns number found (may be 0), -1 on sysfs error. */
int omni_scan(struct omni_device_info *out, int max);

/* Open a device by hidraw node path (e.g. "/dev/hidraw2") or NULL to auto-open
 * the first found. Probes the node with the AA pulse. Returns NULL on error. */
omni_dev *omni_open(const char *hidraw_node);
void      omni_close(omni_dev *dev);

const char *omni_last_error(void);   /* human readable last error */
/* 1 if custom per-key lighting (palette + cmd 0x12) is known to work on
 * this keyboard; 0 on 320F:505B and unknown IDs (see PROTOCOL.md s.13) */
int omni_supports_custom_light(const omni_dev *d);

/* ------------------------------------------------------------------ */
/* Low-level protocol (see PROTOCOL.md §3).  All return OMNI_OK or negative. */

int omni_begin(omni_dev *d);                 /* cmd 01 */
int omni_commit(omni_dev *d);                /* cmd 02 */
int omni_probe(omni_dev *d);                 /* cmd AA */
int omni_ctrl_pulse(omni_dev *d);            /* cmd 1C */

/* generic block transfers (settings NVRAM address space) */
int omni_read_block (omni_dev *d, uint16_t offset, uint8_t *buf, uint16_t len);  /* cmd 05 */
int omni_write_block(omni_dev *d, uint16_t offset, const uint8_t *buf, uint16_t len); /* cmd 06 */

/* small status read: up to 56 bytes from a bank selected by the command */
int omni_read_small(omni_dev *d, uint8_t cmd, uint16_t offset,
                    uint8_t *buf, uint16_t len);   /* cmd 1A style */

/* device info (cmd 03): 34 bytes; [5]=key count, [6]=macro capacity*128 */
int omni_read_info(omni_dev *d, uint8_t info[34]);

/* key-ID table (cmd 07): keycount*3 bytes of [T,X,P] triples */
int omni_read_keyids(omni_dev *d, uint8_t *table, uint16_t len);

/* live assignment bank (cmd 09) and second bank (cmd 27): keycount*3 bytes */
int omni_write_assignments(omni_dev *d, const uint8_t *table, uint16_t len, uint8_t bank);

/* remap layer page (cmd 0A/0B): 512-byte page at layer*0x200 */
int omni_read_remap (omni_dev *d, uint8_t layer, uint8_t *page, uint16_t len);
int omni_write_remap(omni_dev *d, uint8_t layer, const uint8_t *page, uint16_t len);

/* Custom-group colour pages, 320F:505B only (PROTOCOL.md 5.2b).
 * Three 384-byte pages (128 slots x 3 RGB bytes, slot-major) at settings-NVRAM
 * 0x6C0/0x8C0/0xAC0, reachable both through cmd 05/06 and - as the same bytes -
 * through cmd 0A/0B at 0x000/0x200/0x400. */
#define OMNI_GROUP_OFF0   0x6C0u
#define OMNI_GROUP_MAXLEN 384u
#define OMNI_CUSTOM_MODE  20u    /* settings-record mode byte: "Customize" */

struct omni_keymap;   /* defined below */

int  omni_supports_group_rgb(const omni_dev *d);   /* 1 on 320F:505B */
/* cmd 05/06 address of a custom group page (validates group 0..2) */
int  omni_group_off(uint8_t group, uint16_t *off);
int  omni_read_group (omni_dev *d, uint8_t group, uint8_t *page, uint16_t len);
int  omni_write_group(omni_dev *d, uint8_t group, const uint8_t *page,
                      uint16_t len, int via_cmd06);
/* write, then read back and compare the whole page */
int  omni_write_group_verified(omni_dev *d, uint8_t group, const uint8_t *page,
                               uint16_t len, int via_cmd06);
/* set one slot's RGB inside a caller-owned page buffer (len >= (slot+1)*3) */
int  omni_group_set_slot(uint8_t *page, uint16_t len, uint16_t slot,
                         uint8_t r, uint8_t g, uint8_t b);
/* resolve a key index through the map, then set its slot colour */
int  omni_group_set_key(uint8_t *page, uint16_t len, const struct omni_keymap *km,
                        int key, uint8_t r, uint8_t g, uint8_t b);
/* dump a page as "slot col row name RRGGBB" lines to fp */
void omni_group_print(FILE *fp, const uint8_t *page, uint16_t len,
                      const struct omni_keymap *km);

/* per-LED palette index table (cmd 1B read / cmd 12 write): 1 byte per LED */
int omni_read_ledidx (omni_dev *d, uint8_t *tab, uint16_t count);
int omni_write_ledidx(omni_dev *d, const uint8_t *tab, uint16_t count);

/* macro table upload (cmd 15): raw 0x55AA image, see PROTOCOL.md §7 */
int omni_write_macros(omni_dev *d, const uint8_t *image, uint16_t len);

/* 17-byte palette reports (report ID 0xFF), 12 frames — PROTOCOL.md §1.1 */
int omni_send_palette(omni_dev *d, const uint8_t rgb[OMNI_PALETTE_COUNT][3],
                      const uint8_t scale[OMNI_PALETTE_COUNT]);

/* ------------------------------------------------------------------ */
/* High level                                                          */

struct omni_keymap {
    /* key_idx -> device slot (0xFF = unmapped) and reverse */
    uint8_t key_to_slot[OMNI_KEYREF_COUNT];
    uint8_t slot_to_key[OMNI_MAX_KEYS];
};

struct omni_settings {
    uint8_t slot;        /* record[0] */
    uint8_t mode;        /* record[1]  lighting mode 0..31 */
    uint8_t brightness;  /* record[2]  0..4 */
    uint8_t speed;       /* record[3]  0..4 */
    uint8_t fx;          /* record[4]  direction */
    uint8_t multicolor;  /* record[5] */
    uint8_t blue;        /* record[8] */
    uint8_t green;       /* record[9] */
    uint8_t red;         /* record[10] */
    uint8_t zzcc_mode;   /* record[12] */
    uint8_t w_flag;      /* record[13] */
    uint8_t key6_flag;   /* record[14] */
    uint8_t win_flag;    /* record[15] */
    uint8_t polling;     /* record[16]  ms: 1,2,4,8 */
    uint8_t layout;      /* 0: colour B,G,R at [8..10]; 1: R,G,B at [6..8] (320F:505B) */
    uint8_t custom_group;/* record[25] */
    uint8_t raw[OMNI_SETTINGS_LEN];
};

/* load: reads active slot P (offset 0) + 34-byte record at P*0x21 */
int omni_load_settings(omni_dev *d, struct omni_settings *s);
/* save: writes the 34-byte record back at slot*0x21 inside 01..02 session */
int omni_save_settings(omni_dev *d, struct omni_settings *s);
void omni_settings_unpack(const uint8_t rec[OMNI_SETTINGS_LEN], struct omni_settings *s);
void omni_settings_pack(const struct omni_settings *s, uint8_t rec[OMNI_SETTINGS_LEN]);
/* re-read the colour fields of s from s->raw for the given record layout */
void omni_settings_use_layout(struct omni_settings *s, int layout);
int  omni_device_layout(const omni_dev *d);

/* build the key<->slot map from the cmd-07 key-ID table */
int omni_build_keymap(const uint8_t *keyids, uint16_t keycount, struct omni_keymap *km);

/* key reference index for a name ("PageUp", "D", "f12"); -1 if unknown */
int omni_key_index_by_name(const char *name);

/* ---- lighting profiles, backup/restore, verified writes (omni_profile.c) ---- */
struct omni_profile {
    uint8_t mode;        /* 0..30, see omni_mode_names (IDs of default_light.json) */
    uint8_t brightness;  /* 0..4 */
    uint8_t speed;       /* 0..4 */
    uint8_t fx;          /* direction / variant */
    uint8_t multicolor;  /* 0/1 */
    uint8_t red, green, blue;
};
void omni_profile_defaults(struct omni_profile *p);
int  omni_profile_check(const struct omni_profile *p);
void omni_profile_from_settings(const struct omni_settings *s, struct omni_profile *p);
void omni_profile_apply_to(const struct omni_profile *p, struct omni_settings *s);
int  omni_profile_save(const char *name, const struct omni_profile *p);
int  omni_profile_load(const char *name, struct omni_profile *p);
int  omni_profile_list(char (*names)[64], int max);
int  omni_profile_delete(const char *name);
int  omni_parse_color(const char *rrggbb, uint8_t *r, uint8_t *g, uint8_t *b);
/* write the record, then read it back and compare every field */
int  omni_save_settings_verified(omni_dev *d, struct omni_settings *s);
/* save current record + first 256 NVRAM bytes under ~/.config/omni-linux/backups */
int  omni_backup_settings(omni_dev *d, char *path_out, size_t n);
int  omni_read_backup(const char *path, struct omni_settings *s);
/* helpers exported from omni.c */
int  omni_fail(const char *fmt, ...);
void omni_device_id(const omni_dev *d, uint16_t *vid, uint16_t *pid);

/* full lighting apply: mode/brightness/speed/rgb via settings record */
int omni_apply_lighting(omni_dev *d, const struct omni_settings *s);

/* custom per-key lighting: palette + LED index table + mode 17 */
int omni_apply_custom(omni_dev *d, const uint8_t palette[OMNI_PALETTE_COUNT][3],
                      const uint8_t *led_index, uint16_t count, uint8_t group);

/* win-lock status via cmd 1A: returns 0/1 or negative error */
int omni_query_winlock(omni_dev *d);

/* ------------------------------------------------------------------ */
/* Key tables (omni_keys.c)                                            */

extern const uint8_t omni_keyref[OMNI_KEYREF_COUNT][3];  /* reference IDs */
extern const char  *omni_keyref_name[OMNI_KEYREF_COUNT]; /* human names  */
extern const char  *omni_mode_names[32];
int omni_mode_kind(unsigned mode);               /* 0 app-offered, 1 hidden alias, 2 host-fed */
int omni_selectable_modes(uint8_t out[32]);      /* IDs the app offers (minus host-fed) */

/* keyboard visual layout, generated from the original Windows skin
 * (scripts/gen_layout.py) — canvas is 673x218 logical pixels */
#define OMNI_KB_CANVAS_W 673
#define OMNI_KB_CANVAS_H 218
struct omni_key_box { int key_idx; int x, y, w, h; };
extern const struct omni_key_box omni_key_boxes[];
extern const int omni_key_box_count;
/* keys not drawn on the skin image (M1-M5, media row, ...) — extra strip */
extern const int omni_hidden_keys[];
extern const int omni_hidden_key_count;

/* standard HID keyboard usages, for the assignment editor */
struct omni_usage { uint8_t usage; const char *name; };
extern const struct omni_usage omni_usages[];
extern const int omni_usage_count;
extern const char *omni_mod_names[8];   /* LCtrl..RGui, bitmask 01..80 */

/* ------------------------------------------------------------------ */
/* Device introspection & helpers (omni.c)                              */

int      omni_get_info(omni_dev *d, uint8_t info[34]); /* cached cmd 03 */
uint8_t  omni_key_count(const omni_dev *d);            /* info[5]       */
uint16_t omni_macro_capacity(const omni_dev *d);       /* info[6] << 7  */
const char *omni_dev_node(const omni_dev *d);
const char *omni_dev_name(const omni_dev *d);
int      omni_fd(omni_dev *d);               /* raw access for experiments */
void     omni_set_log(omni_dev *d,
                      void (*cb)(void *ctx, int is_rx,
                                 const uint8_t *frame, unsigned len),
                      void *ctx);
void     omni_set_timeout(omni_dev *d, int timeout_ms);

/* human readable text for a 3-byte assignment (and reverse parse;
 * parse accepts "20 00 29" / "0x200029" / key names / "Media 0x1B" /
 * "M3" / "disabled") */
void omni_assignment_text(const uint8_t a[3], char *out, size_t n);
int  omni_assignment_parse(const char *text, uint8_t out[3]);

/* build a 512-byte slot-major remap page from key-indexed assignments;
 * quirks: OMNI_QUIRK_SWAP_ENTER_BACKSLASH reproduces the model-0x14
 * Enter(75)->slot107 / Backslash(55)->slot106 write-path swap */
#define OMNI_QUIRK_SWAP_ENTER_BACKSLASH 1u
int omni_build_remap_page(const uint8_t assignment[][3], uint16_t keys,
                          const struct omni_keymap *km,
                          uint8_t page[OMNI_REMAP_PAGE], unsigned quirks);

#ifdef __cplusplus
}
#endif
#endif /* OMNI_H */
