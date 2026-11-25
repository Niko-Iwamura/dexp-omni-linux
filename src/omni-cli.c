/*
 * omni-cli.c — command line configurator for the DEXP OMNI keyboard
 *
 * Build: make omni-cli
 * Usage: omni-cli <command> [options]
 */
#include "omni.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static omni_dev *must_open(void)
{
    omni_dev *d = omni_open(NULL);
    if (!d) {
        fprintf(stderr, "error: %s\n", omni_last_error());
        exit(1);
    }
    return d;
}

static void print_rc(const char *what, int rc)
{
    if (rc == OMNI_OK)
        printf("%s: OK\n", what);
    else
        printf("%s: FAILED (%d) — %s\n", what, rc, omni_last_error());
}

static void dump_hex(const uint8_t *b, unsigned n, unsigned base)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if (i % 16 == 0) printf("  %04x: ", base + i);
        printf("%02x ", b[i]);
        if (i % 16 == 15 || i + 1 == n) printf("\n");
    }
}

static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0;
    unsigned v;
    while (*s && n < max) {
        while (*s == ' ' || *s == ':' || *s == ',') s++;
        if (!*s) break;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v;
        while (*s && *s != ' ' && *s != ':' && *s != ',') s++;
    }
    return n;
}

static void usage(void)
{
    puts(
"omni-cli — DEXP OMNI keyboard configurator (Linux)\n"
"\n"
"Usage: omni-cli <command> [options]\n"
"\n"
"Commands:\n"
"  scan                                 list HID interfaces of the keyboard\n"
"  info                                 connect + print device info (cmd 03)\n"
"  probe                                send the 0x04/AA probe pulse\n"
"  pulse                                send the 0x04/1C control pulse\n"
"  winlock                              query Windows-lock state (cmd 1A)\n"
"  modes [--all]                        list lighting modes (the keyboard's own IDs)\n"
"  lighting                             show current lighting\n"
"  lighting [--mode N] [--brightness 0-4] [--speed 0-4] [--color RRGGBB]\n"
"           [--multicolor 0|1] [--fx N] [--dry-run] [--no-backup]\n"
"                                       change lighting (backup first, verified after)\n"
"  profile list | show NAME | delete NAME\n"
"  profile new NAME [--mode N ...]      create a profile file (no keyboard needed)\n"
"  profile save NAME                    store the keyboard's current lighting\n"
"  profile apply NAME [--dry-run]       switch the keyboard to a stored profile\n"
"  backup                               save settings record + NVRAM head to a file\n"
"  restore FILE [--dry-run]             write a backed-up settings record back\n"
"  set [--key6 0|1] [--win 0|1] [--wflag 0|1] [--polling 1|2|4|8]\n"
"  dump --offset 0xNN --len N           raw settings-NVRAM read (cmd 05)\n"
"  snapshot FILE [--limit 0xNNNN]       read-only dump of keyboard memory (diff with tools/snapdiff.py)\n"
"  keyids                               dump key-ID/live table (cmd 07)\n"
"  ledidx                               dump per-LED palette table (cmd 1B)\n"
"  assign [--read]                      dump the live key-assignment table (cmd 07)\n"
"  assign --set KEY=ASSIGN [--set ...] [--dry-run]\n"
"                                       rebind keys. On 320F:505B this writes the\n"
"                                       keyboard's PERSISTENT key table (NVRAM bank 0,\n"
"                                       cmd 06) and verifies it; unplug/re-plug to\n"
"                                       activate (the map is loaded at power-on).\n"
"                                       On other models it writes the live bank\n"
"                                       (cmd 09, immediate).\n"
"  remap --layer N --read               dump a remap layer page (cmd 0A)\n"
"  remap --layer N --set KEY=ASSIGN [--set ...] [--dry-run] [--force]\n"
"                                       modify + write a remap layer (cmd 0B).\n"
"                                       On 320F:505B these pages are the lighting\n"
"                                       groups (cmd 0B paints colours) - the tool\n"
"                                       refuses unless --force is given; use\n"
"                                       'assign' for key remaps instead.\n"
"                                       ASSIGN: 'A', 'ctrl+a', 'M3', 'Media 0x1B',\n"
"                                               '20 00 29', '0x200029', 'disabled'\n"
"  group [--group 0-2] [--read]         dump a custom-group RGB page (320F:505B)\n"
"  group --group N --set KEY=RRGGBB [--set ...] [--dry-run]\n"
"                                       paint keys, verified write (cmd 06 @0x6C0)\n"
"  group --group N --from FILE          write a whole 384-byte page from a file\n"
"  group --group N --save FILE          dump the page to FILE (raw binary)\n"
"  group --group N --mode               switch the settings record to Customize\n"
"  raw --frame \"04 ...\"                 send a raw 64-byte frame, print reply\n"
"\n"
"Examples:\n"
"  omni-cli lighting --mode 3 --brightness 4 --speed 2 --color FF6600 --dry-run\n"
"  omni-cli profile save night && omni-cli profile apply night\n"
"  omni-cli assign --set 6=PrintScreen        # remap F6 -> PrintScreen (persistent)\n"
"  omni-cli assign --set 6=20 00 46 --dry-run\n"
"  omni-cli remap --layer 1 --set 120=M5 --set 22=Esc\n"
"  omni-cli group --read                    # dump group 0 per-key colours\n"
"  omni-cli group --group 1 --set PageUp=0200B2 --set D=FF0000\n"
    );
}


/* ---- lighting helpers ------------------------------------------------ */

static const char *mode_name(unsigned m)
{
    return (m < 32 && omni_mode_names[m]) ? omni_mode_names[m] : "?";
}

static void print_settings(const struct omni_settings *s)
{
    printf("slot %u: mode %u (%s), brightness %u, speed %u, fx %u, RGB #%02X%02X%02X, multicolor %u\n",
           s->slot, s->mode, mode_name(s->mode), s->brightness, s->speed, s->fx,
           s->red, s->green, s->blue, s->multicolor);
}

static int diff_settings(const struct omni_settings *a, const struct omni_settings *b)
{
    int n = 0;
#define D(f, nm) do { if (a->f != b->f) { \
        printf("  %-11s %u -> %u\n", nm, a->f, b->f); n++; } } while (0)
    D(mode, "mode"); D(brightness, "brightness"); D(speed, "speed"); D(fx, "fx");
    D(multicolor, "multicolor"); D(red, "red"); D(green, "green"); D(blue, "blue");
    D(zzcc_mode, "zzcc"); D(w_flag, "wflag"); D(key6_flag, "key6");
    D(win_flag, "winlock"); D(polling, "polling"); D(custom_group, "group");
#undef D
    return n;
}

/* modes the Windows app does not offer for this keyboard, or that it feeds itself */
static int check_mode(int mode, int force)
{
    if (mode < 0 || mode > 30) {
        fprintf(stderr, "mode must be 0-30 (list them with: omni-cli modes)\n");
        return -1;
    }
    if (force) return 0;
    if (omni_mode_kind((unsigned)mode) == 2) {
        fprintf(stderr, "mode %d (%s) is fed by the Windows app (audio / screen capture) and does\n"
                        "nothing on its own; use --force to write it anyway\n",
                mode, mode_name((unsigned)mode));
        return -1;
    }
    if (omni_mode_kind((unsigned)mode) == 1) {
        fprintf(stderr, "mode %d (%s) is an alternate ID the Windows app never offers for this\n"
                        "keyboard; see: omni-cli modes   (use --force to write it anyway)\n",
                mode, mode_name((unsigned)mode));
        return -1;
    }
    return 0;
}

/* diff, back up, write, verify by read-back */
static int write_settings(omni_dev *d, const struct omni_settings *before,
                          struct omni_settings *after, int dry, int nobackup)
{
    int rc;

    puts("changes:");
    if (diff_settings(before, after) == 0) { puts("  (none) — nothing to write"); return 0; }
    if (dry) { puts("dry run: nothing written"); return 0; }
    if (!nobackup) {
        char path[640];
        rc = omni_backup_settings(d, path, sizeof path);
        if (rc != OMNI_OK) {
            print_rc("backup (write aborted; --no-backup skips this)", rc);
            return 1;
        }
        printf("backup saved: %s\n", path);
    }
    rc = omni_save_settings_verified(d, after);
    if (rc != OMNI_OK) {
        print_rc("write", rc);
        fprintf(stderr, "to go back: omni-cli restore <backup file>\n");
        return 1;
    }
    puts("written, verified by read-back");
    return 0;
}

static int cmd_modes(int argc, char **argv)
{
    unsigned i;
    int all = argc > 2 && !strcmp(argv[2], "--all");

    for (i = 0; i <= 30; i++) {
        int kind = omni_mode_kind(i);
        if (kind == 1 && !all) continue;
        printf("%2u  %-26s%s\n", i, mode_name(i),
               kind == 1 ? "  (alternate id, hidden in the Windows app)" :
               kind == 2 ? "  (needs the Windows app)" :
               i == 20   ? "  (per-key colours, uses the stored custom group)" : "");
    }
    return 0;
}

static int cmd_profile(int argc, char **argv)
{
    const char *sub = argc > 2 ? argv[2] : "";
    const char *name = argc > 3 ? argv[3] : NULL;
    struct omni_profile p;
    int rc, i;

    if (!strcmp(sub, "list")) {
        char names[64][64];
        int n = omni_profile_list(names, 64);
        if (n < 0) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
        if (n == 0) puts("no profiles yet (create one: omni-cli profile save NAME)");
        for (i = 0; i < n; i++) {
            if (omni_profile_load(names[i], &p) == OMNI_OK)
                printf("%-24s mode %2u %-22s bright %u speed %u #%02X%02X%02X\n", names[i],
                       p.mode, mode_name(p.mode), p.brightness, p.speed, p.red, p.green, p.blue);
            else
                printf("%-24s (unreadable: %s)\n", names[i], omni_last_error());
        }
        return 0;
    }
    if (!name) { fprintf(stderr, "profile %s: NAME missing (see: omni-cli help)\n", sub); return 1; }

    if (!strcmp(sub, "show")) {
        if (omni_profile_load(name, &p) != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
        printf("%s: mode %u (%s), brightness %u, speed %u, fx %u, RGB #%02X%02X%02X, multicolor %u\n",
               name, p.mode, mode_name(p.mode), p.brightness, p.speed, p.fx, p.red, p.green, p.blue, p.multicolor);
        return 0;
    }
    if (!strcmp(sub, "delete")) {
        rc = omni_profile_delete(name);
        if (rc != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
        printf("deleted %s\n", name);
        return 0;
    }
    if (!strcmp(sub, "new")) {         /* build a profile from options, no keyboard needed */
        int allow_host = 0;
        omni_profile_defaults(&p);
        for (i = 4; i < argc; i++) {
            int v;
            if ((!strcmp(argv[i], "--allow-host-modes") || !strcmp(argv[i], "--force"))) allow_host = 1;
            else if (!strcmp(argv[i], "--mode") && i + 1 < argc) { v = atoi(argv[++i]); if (check_mode(v, 1) < 0) return 1; p.mode = (uint8_t)v; }
            else if (!strcmp(argv[i], "--brightness") && i + 1 < argc) p.brightness = (uint8_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--speed") && i + 1 < argc) p.speed = (uint8_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--fx") && i + 1 < argc) p.fx = (uint8_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--multicolor") && i + 1 < argc) p.multicolor = (uint8_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
                if (omni_parse_color(argv[++i], &p.red, &p.green, &p.blue) != 0) { fprintf(stderr, "bad colour (want RRGGBB)\n"); return 1; }
            } else { fprintf(stderr, "profile new: unknown option %s\n", argv[i]); return 1; }
        }
        if (check_mode(p.mode, allow_host) < 0) return 1;
        if (omni_profile_save(name, &p) != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
        printf("saved profile %s\n", name);
        return 0;
    }
    if (!strcmp(sub, "save") || !strcmp(sub, "apply")) {
        struct omni_settings s, before;
        int dry = 0, nobackup = 0, allow_host = 0;
        omni_dev *d;

        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--dry-run")) dry = 1;
            else if (!strcmp(argv[i], "--no-backup")) nobackup = 1;
            else if ((!strcmp(argv[i], "--allow-host-modes") || !strcmp(argv[i], "--force"))) allow_host = 1;
            else { fprintf(stderr, "profile %s: unknown option %s\n", sub, argv[i]); return 1; }
        }
        if (!strcmp(sub, "apply")) {
            if (omni_profile_load(name, &p) != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
            if (check_mode(p.mode, allow_host) < 0) return 1;
        }
        d = must_open();
        rc = omni_load_settings(d, &s);
        if (rc != OMNI_OK) { print_rc("load settings", rc); omni_close(d); return 1; }
        if (!strcmp(sub, "save")) {
            omni_profile_from_settings(&s, &p);
            rc = omni_profile_save(name, &p);
            if (rc != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); omni_close(d); return 1; }
            printf("saved current lighting as profile %s: ", name);
            print_settings(&s);
            omni_close(d);
            return 0;
        }
        before = s;
        omni_profile_apply_to(&p, &s);
        rc = write_settings(d, &before, &s, dry, nobackup);
        omni_close(d);
        return rc;
    }
    fprintf(stderr, "usage: omni-cli profile list | show NAME | new NAME [opts] | save NAME | apply NAME [--dry-run] | delete NAME\n");
    return 1;
}

static int cmd_backup(void)
{
    char path[640];
    omni_dev *d = must_open();
    int rc = omni_backup_settings(d, path, sizeof path);

    omni_close(d);
    if (rc != OMNI_OK) { print_rc("backup", rc); return 1; }
    printf("backup saved: %s\n", path);
    return 0;
}

static int cmd_restore(int argc, char **argv)
{
    struct omni_settings want, cur;
    int dry = 0, i, rc;
    omni_dev *d;

    if (argc < 3) { fprintf(stderr, "usage: omni-cli restore FILE [--dry-run]\n"); return 1; }
    for (i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
    if (omni_read_backup(argv[2], &want) != OMNI_OK) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
    d = must_open();
    omni_settings_use_layout(&want, omni_device_layout(d));
    rc = omni_load_settings(d, &cur);
    if (rc != OMNI_OK) { print_rc("load settings", rc); omni_close(d); return 1; }
    if (cur.slot != want.slot) {
        fprintf(stderr, "backup is for slot %u but the keyboard's active slot is %u; refusing\n", want.slot, cur.slot);
        omni_close(d);
        return 1;
    }
    rc = write_settings(d, &cur, &want, dry, 0);
    omni_close(d);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 0; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "scan")) {
        struct omni_device_info list[16];
        int n = omni_scan(list, 16), i;
        if (n < 0) { fprintf(stderr, "error: %s\n", omni_last_error()); return 1; }
        if (n == 0) { puts("no DEXP OMNI (0c45:1007 / 320f:505b) interfaces found"); return 0; }
        for (i = 0; i < n; i++)
            printf("%s  %04x:%04x  %s\n", list[i].hidraw_node,
                   list[i].vid, list[i].pid, list[i].hid_name);
        return 0;
    }

    if (!strcmp(cmd, "modes"))   return cmd_modes(argc, argv);
    if (!strcmp(cmd, "profile")) return cmd_profile(argc, argv);
    if (!strcmp(cmd, "backup"))  return cmd_backup();
    if (!strcmp(cmd, "restore")) return cmd_restore(argc, argv);

    if (!strcmp(cmd, "raw")) {
        const char *frame = NULL;
        int i;
        for (i = 2; i < argc - 1; i++)
            if (!strcmp(argv[i], "--frame")) frame = argv[++i];
        if (!frame) { fprintf(stderr, "raw: --frame required\n"); return 1; }
        uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
        int n = parse_hex(frame, req, OMNI_REPORT_SIZE);
        if (n <= 0) { fprintf(stderr, "raw: bad hex\n"); return 1; }
        omni_dev *d = must_open();
        omni_set_log(d, NULL, NULL);
        /* send raw: bypass library checksum - write directly */
        int w = (int)write(omni_fd(d), req, OMNI_REPORT_SIZE);
        (void)w;
        printf("request  (%d bytes): ", n);
        for (i = 0; i < OMNI_REPORT_SIZE; i++) printf("%02x ", req[i]);
        printf("\n");
        printf("response: ");
        ssize_t r = read(omni_fd(d), resp, OMNI_REPORT_SIZE);
        if (r < 0) printf("(none: %s)\n", strerror(errno));
        else { for (i = 0; i < r; i++) printf("%02x ", resp[i]); printf("\n"); }
        omni_close(d);
        return 0;
    }

    omni_dev *d = must_open();
    int rc;

    if (!strcmp(cmd, "probe")) {
        print_rc("probe (cmd AA)", omni_probe(d));
    } else if (!strcmp(cmd, "pulse")) {
        print_rc("control pulse (cmd 1C)", omni_ctrl_pulse(d));
    } else if (!strcmp(cmd, "winlock")) {
        rc = omni_query_winlock(d);
        if (rc < 0) print_rc("winlock query (cmd 1A)", rc);
        else printf("win-lock: %s\n", rc ? "LOCKED" : "unlocked");
    } else if (!strcmp(cmd, "info")) {
        uint8_t info[34];
        rc = omni_read_info(d, info);
        if (rc != OMNI_OK) { print_rc("device info (cmd 03)", rc); return 1; }
        printf("node:            %s\n", omni_dev_node(d));
        printf("name:            %s\n", omni_dev_name(d));
        printf("model id:        %u (0x%02x)\n", info[0], info[0]);
        printf("key/slot count:  %u\n", info[5]);
        printf("macro capacity:  %u bytes\n", info[6] << 7);
        printf("raw:             ");
        for (int i = 0; i < 34; i++) printf("%02x ", info[i]);
        printf("\n");
    } else if (!strcmp(cmd, "lighting")) {
        struct omni_settings s, before;
        int mode = -1, bright = -1, speed = -1, multi = -1, fx = -1;
        int dry = 0, nobackup = 0, allow_host = 0, want = 0;
        const char *color = NULL;
        uint8_t cr = 0, cg = 0, cb = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--read")) { /* default behaviour */ }
            else if (!strcmp(argv[i], "--dry-run")) dry = 1;
            else if (!strcmp(argv[i], "--no-backup")) nobackup = 1;
            else if ((!strcmp(argv[i], "--allow-host-modes") || !strcmp(argv[i], "--force"))) allow_host = 1;
            else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--brightness") && i + 1 < argc) bright = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--fx") && i + 1 < argc) fx = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--multicolor") && i + 1 < argc) multi = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--color") && i + 1 < argc) color = argv[++i];
            else { fprintf(stderr, "lighting: unknown or incomplete option %s\n", argv[i]); omni_close(d); return 1; }
        }
        if (mode >= 0 && check_mode(mode, allow_host) < 0) { omni_close(d); return 1; }
        if (mode < 0 && argc > 2) { /* mode may stay unset */ }
        if ((bright > 4) || (speed > 4) || (multi > 1) || (fx > 255) ||
            (color && omni_parse_color(color, &cr, &cg, &cb) != 0)) {
            fprintf(stderr, "lighting: brightness/speed are 0-4, multicolor 0/1, colour RRGGBB\n");
            omni_close(d);
            return 1;
        }
        rc = omni_load_settings(d, &s);
        if (rc != OMNI_OK) { print_rc("load settings", rc); omni_close(d); return 1; }
        before = s;
        printf("now: "); print_settings(&s);
        if (mode >= 0) { s.mode = (uint8_t)mode; want = 1; }
        if (bright >= 0) { s.brightness = (uint8_t)bright; want = 1; }
        if (speed >= 0) { s.speed = (uint8_t)speed; want = 1; }
        if (fx >= 0) { s.fx = (uint8_t)fx; want = 1; }
        if (multi >= 0) { s.multicolor = (uint8_t)multi; want = 1; }
        if (color) { s.red = cr; s.green = cg; s.blue = cb; want = 1; }
        if (want) {
            int r = write_settings(d, &before, &s, dry, nobackup);
            omni_close(d);
            return r;
        }
    } else if (!strcmp(cmd, "set")) {
        struct omni_settings s;
        int key6 = -1, win = -1, wflag = -1, poll = -1;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--key6") && i + 1 < argc) key6 = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--win") && i + 1 < argc) win = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--wflag") && i + 1 < argc) wflag = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--polling") && i + 1 < argc) poll = atoi(argv[++i]);
        }
        if (key6 < 0 && win < 0 && wflag < 0 && poll < 0) {
            fprintf(stderr, "set: nothing to do (use --key6/--win/--wflag/--polling)\n");
            return 1;
        }
        rc = omni_load_settings(d, &s);
        if (rc != OMNI_OK) { print_rc("load settings", rc); return 1; }
        struct omni_settings before = s;
        if (key6 >= 0) s.key6_flag = key6;
        if (win >= 0) s.win_flag = win;
        if (wflag >= 0) s.w_flag = wflag;
        if (poll > 0) s.polling = poll;
        { int r = write_settings(d, &before, &s, 0, 0); omni_close(d); return r; }
    } else if (!strcmp(cmd, "snapshot")) {
        /* read-only dump for diffing: device info, key IDs, LED table and the
         * cmd 05 / cmd 0A address spaces. Take one before and one after a change
         * made in the Windows app, then compare with tools/snapdiff.py to see
         * where that setting lives in the keyboard's memory. */
        const char *path = argc > 2 ? argv[2] : NULL;
        unsigned limit = 0x4000, off, kc;
        static const uint8_t spaces[2] = { 0x05, 0x0A };
        uint8_t buf[56], tab[512], info[34];
        FILE *f;

        for (int i = 3; i < argc; i++)
            if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = (unsigned)strtoul(argv[++i], NULL, 0);
        if (!path || path[0] == '-') {
            fprintf(stderr, "usage: omni-cli snapshot FILE [--limit 0xNNNN]\n");
            omni_close(d);
            return 1;
        }
        if (limit > 0xFF00) limit = 0xFF00;
        f = fopen(path, "w");
        if (!f) { fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno)); omni_close(d); return 1; }
        fprintf(f, "# omni snapshot v1 (read-only dump)\n");

        if (omni_read_info(d, info) == OMNI_OK) {
            fprintf(f, "03 0000 ");
            for (int i = 0; i < 34; i++) fprintf(f, "%02x", info[i]);
            fputc('\n', f);
        }
        kc = omni_key_count(d);
        if (kc && kc * 3 <= sizeof tab && omni_read_cmd(d, 0x07, 0, tab, (uint16_t)(kc * 3)) == OMNI_OK) {
            fprintf(f, "07 0000 ");
            for (unsigned i = 0; i < kc * 3; i++) fprintf(f, "%02x", tab[i]);
            fputc('\n', f);
        }
        if (kc && kc <= sizeof tab && omni_read_cmd(d, 0x1B, 0, tab, (uint16_t)kc) == OMNI_OK) {
            fprintf(f, "1b 0000 ");
            for (unsigned i = 0; i < kc; i++) fprintf(f, "%02x", tab[i]);
            fputc('\n', f);
        }
        for (int sp = 0; sp < 2; sp++) {
            unsigned ok = 0, errs = 0;
            for (off = 0; off < limit; off += 56) {
                int r = omni_read_cmd(d, spaces[sp], (uint16_t)off, buf, 56);
                if (r != OMNI_OK) {
                    fprintf(f, "%02x %04x ERR\n", spaces[sp], off);
                    if (++errs >= 4) break;
                    continue;
                }
                errs = 0;
                ok++;
                fprintf(f, "%02x %04x ", spaces[sp], off);
                for (int i = 0; i < 56; i++) fprintf(f, "%02x", buf[i]);
                fputc('\n', f);
            }
            printf("cmd %02x: %u blocks of 56 bytes read%s\n", spaces[sp], ok,
                   errs >= 4 ? " (stopped at the first unreadable area)" : "");
        }
        fclose(f);
        printf("snapshot written to %s\n", path);
    } else if (!strcmp(cmd, "dump")) {
        unsigned off = 0, len = 64;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--offset") && i + 1 < argc) off = strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--len") && i + 1 < argc) len = strtoul(argv[++i], NULL, 0);
        }
        if (len > 1024) len = 1024;
        uint8_t buf[1024];
        rc = omni_read_block(d, (uint16_t)off, buf, (uint16_t)len);
        if (rc != OMNI_OK) { print_rc("read block (cmd 05)", rc); return 1; }
        dump_hex(buf, len, off);
    } else if (!strcmp(cmd, "keyids")) {
        uint8_t info[34];
        rc = omni_read_info(d, info);
        if (rc != OMNI_OK) { print_rc("device info", rc); return 1; }
        unsigned kc = info[5];
        uint8_t tab[512];
        rc = omni_begin(d);
        if (rc == OMNI_OK) rc = omni_read_keyids(d, tab, (uint16_t)(kc * 3));
        if (rc == OMNI_OK) rc = omni_commit(d);
        if (rc != OMNI_OK) { print_rc("key-ID table (cmd 07)", rc); return 1; }
        for (unsigned s = 0; s < kc; s++) {
            char txt[64];
            omni_assignment_text(tab + s * 3, txt, sizeof txt);
            printf("slot %3u: %02x %02x %02x   %s\n", s,
                   tab[s*3], tab[s*3+1], tab[s*3+2], txt);
        }
    } else if (!strcmp(cmd, "ledidx")) {
        uint8_t info[34];
        rc = omni_read_info(d, info);
        if (rc != OMNI_OK) { print_rc("device info", rc); return 1; }
        unsigned kc = info[5];
        uint8_t tab[256];
        rc = omni_begin(d);
        if (rc == OMNI_OK) rc = omni_read_ledidx(d, tab, (uint16_t)kc);
        if (rc == OMNI_OK) rc = omni_commit(d);
        if (rc != OMNI_OK) { print_rc("LED table (cmd 1B)", rc); return 1; }
        printf("per-LED palette indices (slot-major):\n");
        dump_hex(tab, kc, 0);
    } else if (!strcmp(cmd, "remap") || !strcmp(cmd, "assign")) {
        int live = !strcmp(cmd, "assign") ? 1 : 0;
        uint8_t info[34];
        int layer = 0, do_read = 0, dry = 0, force = 0;
        const char *sets[64]; int nsets = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--layer") && i + 1 < argc) layer = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--read")) do_read = 1;
            else if (!strcmp(argv[i], "--live")) live = 1;
            else if (!strcmp(argv[i], "--dry-run")) dry = 1;
            else if (!strcmp(argv[i], "--force")) force = 1;
            else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 64) sets[nsets++] = argv[++i];
            else { fprintf(stderr, "remap: unknown or incomplete option %s\n", argv[i]); return 1; }
        }
        rc = omni_read_info(d, info);
        if (rc != OMNI_OK) { print_rc("device info", rc); omni_close(d); return 1; }
        unsigned kc = info[5];
        if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;
        int group_rgb = omni_supports_group_rgb(d);

        /* On 320F:505B the cmd 0A/0B "remap layers" are the three custom
         * colour-group pages, not key assignments (PROTOCOL.md 5.2b / 13.2).
         * Writing assignments there paints junk colours into the lighting
         * table - refuse unless the caller insists via --force. */
        if (!live && group_rgb && nsets > 0 && !force) {
            fprintf(stderr,
                "remap: this keyboard's cmd 0A/0B pages are lighting groups, not key\n"
                "assignments. Use `omni-cli assign --set ...` (writes the persistent\n"
                "key table), or pass --force to write cmd 0B anyway (paints colours,\n"
                "damages lighting).\n");
            omni_close(d);
            return 1;
        }

        /* Assignments live in the cmd 07 / cmd 09 table. The cmd 0A page is
         * only meaningful on hardware where it really is a remap layer. */
        uint8_t table[OMNI_REMAP_PAGE];
        memset(table, 0, sizeof table);
        rc = omni_begin(d);
        if (rc == OMNI_OK) rc = omni_read_keyids(d, table, (uint16_t)(kc * 3));
        if (rc == OMNI_OK) rc = omni_commit(d);
        if (rc != OMNI_OK) { print_rc("key-assignment table (cmd 07)", rc); omni_close(d); return 1; }

        struct omni_keymap km;
        omni_build_keymap(table, (uint16_t)kc, &km);

        /* Only read the cmd 0A layer page when it is a genuine remap layer. */
        if (!live && !group_rgb) {
            rc = omni_read_remap(d, (uint8_t)layer, table, (uint16_t)(kc * 3));
            if (rc != OMNI_OK) { print_rc("read remap page (cmd 0A)", rc); omni_close(d); return 1; }
            omni_build_keymap(table, (uint16_t)kc, &km);
        }

        if (nsets > 0) {
            for (int i = 0; i < nsets; i++) {
                char buf[128];
                snprintf(buf, sizeof buf, "%s", sets[i]);
                char *eq = strchr(buf, '=');
                if (!eq) { fprintf(stderr, "bad --set '%s' (need KEY=ASSIGN)\n", sets[i]); continue; }
                *eq = 0;
                int key = atoi(buf);
                if (key < 0 || key >= OMNI_KEYREF_COUNT) {
                    fprintf(stderr, "bad --set '%s': key index must be 0-%d\n",
                            sets[i], OMNI_KEYREF_COUNT - 1);
                    continue;
                }
                uint8_t a[3];
                if (omni_assignment_parse(eq + 1, a) != OMNI_OK) {
                    fprintf(stderr, "bad assignment '%s': %s\n", eq + 1, omni_last_error());
                    continue;
                }
                int slot = km.key_to_slot[key];
                if (slot == 0xFF) {
                    fprintf(stderr, "key %d (%s) has no slot on this keyboard\n", key,
                            (key < OMNI_KEYREF_COUNT && omni_keyref_name[key])
                                ? omni_keyref_name[key] : "?");
                    continue;
                }
                memcpy(table + slot * 3, a, 3);  /* overlay: keep every other slot */
                printf("key %d %-10s slot %3d -> %02x %02x %02x\n", key,
                       (key < OMNI_KEYREF_COUNT && omni_keyref_name[key])
                           ? omni_keyref_name[key] : "?",
                       slot, a[0], a[1], a[2]);
            }
            if (dry) {
                printf("dry-run: would write %u bytes to the %s\n", kc * 3,
                       (group_rgb && !(force && !live))
                           ? "persistent key table (NVRAM 0x0C0, cmd 06)"
                           : (live ? "live assignment bank (cmd 09)"
                                   : "remap layer page (cmd 0B)"));
            } else if (group_rgb && !(force && !live)) {
                /* 320F:505B: the real, persistent key table; the running map is
                 * loaded from it at power-on (PROTOCOL.md 13.3). */
                uint8_t back[OMNI_REMAP_PAGE];
                rc = omni_write_assign_bank(d, 0, table, (uint16_t)(kc * 3), back);
                print_rc("write persistent key table (NVRAM 0x0C0, cmd 06)", rc);
                if (rc == OMNI_OK) {
                    printf("saved and verified. This keyboard loads its key map at "
                           "power-on:\nunplug and re-plug it to activate.\n");
                } else {
                    omni_close(d);
                    return 1;
                }
            } else if (live) {
                rc = omni_begin(d);
                if (rc == OMNI_OK)
                    rc = omni_write_assignments(d, table, (uint16_t)(kc * 3), 0);
                if (rc == OMNI_OK) rc = omni_commit(d);
                print_rc("write live assignment bank (cmd 09)", rc);
                if (rc != OMNI_OK) { omni_close(d); return 1; }
            } else {
                rc = omni_begin(d);
                if (rc == OMNI_OK)
                    rc = omni_write_remap(d, (uint8_t)layer, table, (uint16_t)(kc * 3));
                if (rc == OMNI_OK) rc = omni_commit(d);
                print_rc("write remap page (cmd 0B)", rc);
                if (rc != OMNI_OK) { omni_close(d); return 1; }
            }
        }
        /* dump */
        for (unsigned s = 0; s < kc; s++) {
            char txt[64];
            omni_assignment_text(table + s * 3, txt, sizeof txt);
            int key = km.slot_to_key[s];
            printf("slot %3u (key %3d %-10s): %02x %02x %02x   %s\n", s,
                   key, (key < OMNI_KEYREF_COUNT && omni_keyref_name[key]) ? omni_keyref_name[key] : "?",
                   table[s*3], table[s*3+1], table[s*3+2], txt);
        }
        (void)do_read;
        omni_close(d);
        return 0;
    } else if (!strcmp(cmd, "group")) {
        /* custom-group RGB pages, 320F:505B only (PROTOCOL.md 5.2b) */
        uint8_t info[34];
        int group = 0, dry = 0, want_mode = 0, do_read = 1;
        const char *from = NULL, *save = NULL;
        const char *sets[128]; int nsets = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--group") && i + 1 < argc) group = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--read")) do_read = 1;
            else if (!strcmp(argv[i], "--dry-run")) dry = 1;
            else if (!strcmp(argv[i], "--mode")) want_mode = 1;
            else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
            else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
            else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 128) sets[nsets++] = argv[++i];
            else { fprintf(stderr, "group: unknown or incomplete option %s\n", argv[i]); return 1; }
        }
        if (group < 0 || group > 2) {
            fprintf(stderr, "group: --group must be 0-2\n");
            return 1;
        }
        if (from && (nsets || want_mode)) {
            fprintf(stderr, "group: --from replaces the whole page; do not combine it with --set/--mode\n");
            return 1;
        }
        if (!omni_supports_group_rgb(d)) {
            fprintf(stderr, "group: custom-group RGB pages are only known on "
                            "320F:505B (PROTOCOL.md 5.2b)\n");
            omni_close(d);
            return 1;
        }

        rc = omni_read_info(d, info);
        if (rc != OMNI_OK) { print_rc("device info", rc); omni_close(d); return 1; }
        unsigned kc = info[5];
        if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;

        struct omni_keymap km;
        memset(&km, 0, sizeof km);
        {
            uint8_t keyids[512];
            rc = omni_begin(d);
            if (rc == OMNI_OK) rc = omni_read_keyids(d, keyids, (uint16_t)(kc * 3));
            if (rc == OMNI_OK) rc = omni_commit(d);
            if (rc != OMNI_OK) { print_rc("key-ID table (cmd 07)", rc); omni_close(d); return 1; }
            omni_build_keymap(keyids, (uint16_t)kc, &km);
        }

        uint16_t plen = (uint16_t)(kc * 3);
        if (plen > OMNI_GROUP_MAXLEN) plen = OMNI_GROUP_MAXLEN;
        uint8_t page[OMNI_GROUP_MAXLEN];

        if (from) {
            FILE *f = fopen(from, "rb");
            if (!f) { fprintf(stderr, "group: cannot open %s: %s\n", from, strerror(errno)); omni_close(d); return 1; }
            size_t got = fread(page, 1, plen, f);
            fclose(f);
            if (got != plen) {
                fprintf(stderr, "group: %s holds %zu bytes, need %u\n", from, got, plen);
                omni_close(d);
                return 1;
            }
            if (dry) {
                printf("dry-run: would write %u bytes from %s to group %d (0x%03X)\n",
                       plen, from, group, OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE);
                omni_close(d);
                return 0;
            }
            rc = omni_write_group_verified(d, (uint8_t)group, page, plen, 1);
            print_rc("write group page (cmd 06)", rc);
            omni_close(d);
            return rc == OMNI_OK ? 0 : 1;
        }

        rc = omni_read_group(d, (uint8_t)group, page, plen);
        if (rc != OMNI_OK) { print_rc("read group page (cmd 05)", rc); omni_close(d); return 1; }

        if (save) {
            FILE *f = fopen(save, "wb");
            if (!f) { fprintf(stderr, "group: cannot write %s: %s\n", save, strerror(errno)); omni_close(d); return 1; }
            size_t put = fwrite(page, 1, plen, f);
            fclose(f);
            if (put != plen) { fprintf(stderr, "group: short write to %s\n", save); omni_close(d); return 1; }
            printf("group %d: %u bytes saved to %s\n", group, plen, save);
        }

        for (int i = 0; i < nsets; i++) {
            char buf[128];
            snprintf(buf, sizeof buf, "%s", sets[i]);
            char *eq = strchr(buf, '=');
            if (!eq) { fprintf(stderr, "bad --set '%s' (need KEY=RRGGBB)\n", sets[i]); omni_close(d); return 1; }
            *eq = 0;
            uint8_t r, g, b;
            if (omni_parse_color(eq + 1, &r, &g, &b) != OMNI_OK) {
                fprintf(stderr, "bad colour '%s' (want RRGGBB)\n", eq + 1);
                omni_close(d);
                return 1;
            }
            int key = -1;
            char *end = NULL;
            long as_num = strtol(buf, &end, 10);
            if (end && *end == 0 && as_num >= 0 && as_num < OMNI_KEYREF_COUNT) key = (int)as_num;
            else key = omni_key_index_by_name(buf);
            if (key < 0) { fprintf(stderr, "unknown key '%s'\n", buf); omni_close(d); return 1; }
            int slot = km.key_to_slot[key];
            if (slot == 0xFF || (unsigned)slot >= kc) {
                fprintf(stderr, "key '%s' (idx %d) has no slot on this keyboard\n", buf, key);
                omni_close(d);
                return 1;
            }
            const uint8_t *old = page + slot * 3;
            uint8_t was[3] = { old[0], old[1], old[2] };
            if (omni_group_set_slot(page, plen, (uint16_t)slot, r, g, b) != OMNI_OK) {
                fprintf(stderr, "group: %s\n", omni_last_error());
                omni_close(d);
                return 1;
            }
            printf("set %s (key %d, slot %d): %02X%02X%02X -> %02X%02X%02X\n",
                   omni_keyref_name[key], key, slot, was[0], was[1], was[2], r, g, b);
        }

        int changed = nsets > 0 || want_mode;
        if (changed && !dry) {
            if (nsets > 0) {
                rc = omni_write_group_verified(d, (uint8_t)group, page, plen, 1);
                print_rc("write group page (cmd 06 + read-back)", rc);
                if (rc != OMNI_OK) { omni_close(d); return 1; }
            }
            if (want_mode) {
                struct omni_settings s;
                rc = omni_load_settings(d, &s);
                if (rc != OMNI_OK) { print_rc("load settings", rc); omni_close(d); return 1; }
                if (s.mode != OMNI_CUSTOM_MODE) {
                    s.mode = OMNI_CUSTOM_MODE;
                    s.custom_group = (uint8_t)group;
                    rc = omni_save_settings_verified(d, &s);
                    print_rc("switch to Customize (mode 20)", rc);
                    if (rc != OMNI_OK) { omni_close(d); return 1; }
                } else if (s.custom_group != (uint8_t)group) {
                    s.custom_group = (uint8_t)group;
                    rc = omni_save_settings_verified(d, &s);
                    print_rc("select custom group", rc);
                    if (rc != OMNI_OK) { omni_close(d); return 1; }
                } else {
                    printf("already in Customize, group %d\n", group);
                }
            }
        } else if (dry && changed) {
            printf("dry-run: %d key(s) changed in the buffer, nothing written\n", nsets);
        }

        if (do_read) {
            printf("group %d (cmd 05 0x%03X, %u slots):\n", group,
                   (unsigned)(OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE), plen / 3);
            omni_group_print(stdout, page, plen, &km);
        }
    } else {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        usage();
        omni_close(d);
        return 1;
    }

    omni_close(d);
    return 0;
}
