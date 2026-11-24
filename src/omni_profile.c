/*
 * omni_profile.c — host-side lighting profiles, settings backup/restore and
 * verified writes for the DEXP OMNI keyboard.
 *
 * A profile is a small text file (~/.config/omni-linux/profiles/NAME.profile)
 * holding the lighting fields of the settings record: mode, brightness,
 * speed, direction (fx), multicolour flag and RGB. Applying one changes only
 * those fields; polling rate, Win-lock, 6KRO and the reserved bytes of the
 * record are left exactly as the keyboard has them.
 *
 * Before the first write of a session the CLI saves the current record and
 * the first 256 bytes of settings NVRAM to ~/.config/omni-linux/backups/.
 */
#define _DEFAULT_SOURCE
#include "omni.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- paths ---------------------------------------------------------- */

/* run through sudo: keep our files owned by the invoking user */
static void fix_owner(const char *path)
{
    const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    if (geteuid() == 0 && u && g)
        if (chown(path, (uid_t)atol(u), (gid_t)atol(g)) != 0) { /* best effort */ }
}

static const char *home_dir(void)
{
    const char *su = getenv("SUDO_USER");
    if (geteuid() == 0 && su && *su) {
        struct passwd *pw = getpwnam(su);
        if (pw && pw->pw_dir) return pw->pw_dir;
    }
    return getenv("HOME");
}

static int mkdir_p(const char *path)
{
    char tmp[512];
    size_t i, len = strlen(path);

    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, 0755) == 0) fix_owner(tmp);
            else if (errno != EEXIST) return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

static int config_dir(const char *sub, char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = home_dir();
    int w;

    if (geteuid() != 0 && xdg && *xdg)
        w = snprintf(out, n, "%s/omni-linux/%s", xdg, sub);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/omni-linux/%s", home, sub);
    else
        return omni_fail("cannot find a config directory (HOME is not set)");
    if (w < 0 || (size_t)w >= n) return omni_fail("config path too long");
    if (mkdir_p(out) != 0)
        return omni_fail("cannot create %s: %s", out, strerror(errno));
    return OMNI_OK;
}

static int valid_name(const char *name)
{
    size_t i, len = name ? strlen(name) : 0;

    if (!len || len > 48 || name[0] == '.') return 0;
    for (i = 0; i < len; i++)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_' ||
              name[i] == '-' || name[i] == '.'))
            return 0;
    return 1;
}

static int profile_path(const char *name, char *out, size_t n)
{
    char dir[512];
    int rc;

    if (!valid_name(name))
        return omni_fail("invalid profile name \"%s\" (use letters, digits, '_', '-', '.'; max 48)",
                         name ? name : "");
    if ((rc = config_dir("profiles", dir, sizeof dir)) != OMNI_OK) return rc;
    if (snprintf(out, n, "%s/%s.profile", dir, name) >= (int)n)
        return omni_fail("profile path too long");
    return OMNI_OK;
}

/* ---- profiles ------------------------------------------------------- */

int omni_profile_check(const struct omni_profile *p)
{
    if (p->mode > 30) return omni_fail("profile: mode %u out of range (0-30)", p->mode);
    if (p->brightness > 4) return omni_fail("profile: brightness %u out of range (0-4)", p->brightness);
    if (p->speed > 4) return omni_fail("profile: speed %u out of range (0-4)", p->speed);
    if (p->multicolor > 1) return omni_fail("profile: multicolor must be 0 or 1");
    return OMNI_OK;
}

void omni_profile_defaults(struct omni_profile *p)
{
    memset(p, 0, sizeof *p);
    p->mode = 6;            /* Light On Constantly */
    p->brightness = 4;
    p->speed = 2;
    p->red = p->green = p->blue = 0xFF;
}

void omni_profile_from_settings(const struct omni_settings *s, struct omni_profile *p)
{
    p->mode = s->mode;       p->brightness = s->brightness;
    p->speed = s->speed;     p->fx = s->fx;
    p->multicolor = s->multicolor;
    p->red = s->red;         p->green = s->green;   p->blue = s->blue;
}

void omni_profile_apply_to(const struct omni_profile *p, struct omni_settings *s)
{
    s->mode = p->mode;       s->brightness = p->brightness;
    s->speed = p->speed;     s->fx = p->fx;
    s->multicolor = p->multicolor;
    s->red = p->red;         s->green = p->green;   s->blue = p->blue;
}

int omni_parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned v;
    char *end;

    if (!s) return -1;
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    v = (unsigned)strtoul(s, &end, 16);
    if (*end) return -1;
    *r = (uint8_t)(v >> 16); *g = (uint8_t)(v >> 8); *b = (uint8_t)v;
    return 0;
}

int omni_profile_save(const char *name, const struct omni_profile *p)
{
    char path[640];
    FILE *f;
    int rc;

    if ((rc = omni_profile_check(p)) != OMNI_OK) return rc;
    if ((rc = profile_path(name, path, sizeof path)) != OMNI_OK) return rc;
    f = fopen(path, "w");
    if (!f) return omni_fail("cannot write %s: %s", path, strerror(errno));
    fprintf(f, "# omni-linux lighting profile\n"
               "mode=%u\nbrightness=%u\nspeed=%u\nfx=%u\nmulticolor=%u\ncolor=%02X%02X%02X\n",
            p->mode, p->brightness, p->speed, p->fx, p->multicolor,
            p->red, p->green, p->blue);
    if (fclose(f) != 0) return omni_fail("cannot write %s: %s", path, strerror(errno));
    fix_owner(path);
    return OMNI_OK;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int num_field(const char *key, const char *v, unsigned max, uint8_t *out)
{
    char *end;
    unsigned long n = strtoul(v, &end, 10);

    if (!*v || *end || n > max)
        return omni_fail("profile: bad value \"%s\" for %s (0-%u)", v, key, max);
    *out = (uint8_t)n;
    return OMNI_OK;
}

int omni_profile_load(const char *name, struct omni_profile *p)
{
    char path[640], line[160];
    FILE *f;
    int rc;

    if ((rc = profile_path(name, path, sizeof path)) != OMNI_OK) return rc;
    f = fopen(path, "r");
    if (!f) return omni_fail("no such profile \"%s\" (%s)", name, strerror(errno));
    omni_profile_defaults(p);
    while (fgets(line, sizeof line, f)) {
        char *k = trim(line), *eq, *v;

        if (!*k || *k == '#') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        v = trim(eq + 1);
        k = trim(k);
        rc = OMNI_OK;
        if      (!strcmp(k, "mode"))       rc = num_field(k, v, 255, &p->mode);
        else if (!strcmp(k, "brightness")) rc = num_field(k, v, 255, &p->brightness);
        else if (!strcmp(k, "speed"))      rc = num_field(k, v, 255, &p->speed);
        else if (!strcmp(k, "fx"))         rc = num_field(k, v, 255, &p->fx);
        else if (!strcmp(k, "multicolor")) rc = num_field(k, v, 255, &p->multicolor);
        else if (!strcmp(k, "color")) {
            if (omni_parse_color(v, &p->red, &p->green, &p->blue) != 0)
                rc = omni_fail("profile: bad colour \"%s\" (want RRGGBB)", v);
        }
        if (rc != OMNI_OK) { fclose(f); return rc; }
    }
    fclose(f);
    return omni_profile_check(p);
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int omni_profile_list(char (*names)[64], int max)
{
    char dir[512];
    DIR *dp;
    struct dirent *e;
    int n = 0;

    if (config_dir("profiles", dir, sizeof dir) != OMNI_OK) return -1;
    dp = opendir(dir);
    if (!dp) return -1;
    while ((e = readdir(dp)) && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 8 && len - 8 < 64 && !strcmp(e->d_name + len - 8, ".profile")) {
            memcpy(names[n], e->d_name, len - 8);
            names[n][len - 8] = 0;
            n++;
        }
    }
    closedir(dp);
    qsort(names, (size_t)n, 64, cmp_names);
    return n;
}

int omni_profile_delete(const char *name)
{
    char path[640];
    int rc = profile_path(name, path, sizeof path);

    if (rc != OMNI_OK) return rc;
    if (unlink(path) != 0)
        return omni_fail("cannot delete profile \"%s\": %s", name, strerror(errno));
    return OMNI_OK;
}

/* ---- verified write ------------------------------------------------- */

int omni_save_settings_verified(omni_dev *d, struct omni_settings *s)
{
    struct omni_settings back;
    int rc;

    if ((rc = omni_save_settings(d, s)) != OMNI_OK) return rc;
    if ((rc = omni_load_settings(d, &back)) != OMNI_OK) return rc;
#define CHK(f) do { if (back.f != s->f) \
    return omni_fail("write verification failed: %s wrote %u, read back %u", #f, s->f, back.f); } while (0)
    CHK(slot); CHK(mode); CHK(brightness); CHK(speed); CHK(fx); CHK(multicolor);
    CHK(red); CHK(green); CHK(blue); CHK(zzcc_mode); CHK(w_flag); CHK(key6_flag);
    CHK(win_flag); CHK(polling); CHK(custom_group);
#undef CHK
    return OMNI_OK;
}

/* ---- backup / restore ----------------------------------------------- */

static void put_hex(FILE *f, const uint8_t *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}

int omni_backup_settings(omni_dev *d, char *path_out, size_t n)
{
    struct omni_settings s;
    uint8_t rec[OMNI_SETTINGS_LEN], nv[256];
    unsigned got = 0, off;
    char dir[512], path[640];
    uint16_t vid = 0, pid = 0;
    time_t t = time(NULL);
    struct tm tm;
    FILE *f = NULL;
    int rc, i;

    if ((rc = omni_load_settings(d, &s)) != OMNI_OK) return rc;
    omni_settings_pack(&s, rec);
    for (off = 0; off < sizeof nv; off += 32) {
        if (omni_read_block(d, (uint16_t)off, nv + off, 32) != OMNI_OK) break;
        got = off + 32;
    }
    if ((rc = config_dir("backups", dir, sizeof dir)) != OMNI_OK) return rc;
    omni_device_id(d, &vid, &pid);
    localtime_r(&t, &tm);
    for (i = 0; i < 20 && !f; i++) {
        if (i == 0)
            snprintf(path, sizeof path, "%s/settings-%04d%02d%02d-%02d%02d%02d.txt", dir,
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        else
            snprintf(path, sizeof path, "%s/settings-%04d%02d%02d-%02d%02d%02d-%d.txt", dir,
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, i);
        f = fopen(path, "wx");
        if (!f && errno != EEXIST) break;
    }
    if (!f) return omni_fail("cannot create backup in %s: %s", dir, strerror(errno));
    fprintf(f, "# omni-linux settings backup (restore with: omni-cli restore FILE)\n"
               "device=%04x:%04x\nslot=%u\nrecord=", vid, pid, s.slot);
    put_hex(f, rec, OMNI_SETTINGS_LEN);
    fprintf(f, "\nnvram_len=%u\nnvram=", got);
    put_hex(f, nv, got);
    fputc('\n', f);
    if (fclose(f) != 0) return omni_fail("cannot write %s: %s", path, strerror(errno));
    fix_owner(path);
    snprintf(path_out, n, "%s", path);
    return OMNI_OK;
}

int omni_read_backup(const char *path, struct omni_settings *s)
{
    char line[1024];
    uint8_t rec[OMNI_SETTINGS_LEN];
    int have_rec = 0, slot = -1;
    FILE *f = fopen(path, "r");

    if (!f) return omni_fail("cannot open %s: %s", path, strerror(errno));
    while (fgets(line, sizeof line, f)) {
        char *k = trim(line), *eq;

        if (!*k || *k == '#') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(k, "slot")) {
            slot = atoi(eq + 1);
        } else if (!strcmp(k, "record")) {
            const char *h = trim(eq + 1);
            unsigned i, v;
            if (strlen(h) != OMNI_SETTINGS_LEN * 2) { fclose(f); return omni_fail("backup: bad record length"); }
            for (i = 0; i < OMNI_SETTINGS_LEN; i++) {
                if (sscanf(h + 2 * i, "%2x", &v) != 1) { fclose(f); return omni_fail("backup: bad record hex"); }
                rec[i] = (uint8_t)v;
            }
            have_rec = 1;
        }
    }
    fclose(f);
    if (!have_rec) return omni_fail("backup: no record= line in %s", path);
    if (slot >= 0 && slot != rec[0]) return omni_fail("backup: slot %d does not match record byte 0 (%u)", slot, rec[0]);
    omni_settings_unpack(rec, s);
    return OMNI_OK;
}
