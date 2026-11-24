/*
 * omni.c — DEXP OMNI keyboard Linux driver library
 *
 * Clean-room implementation of the wire protocol used by the Windows
 * "OMNI.exe" configurator (DEXP-branded Eevision/Acrox software,
 * platform "eevision_A_kb_001_20180108_ak60", Sonix VS11K09A MCU).
 * Every frame format below is documented with evidence in PROTOCOL.md.
 *
 * Transport on Linux: a /dev/hidraw node belonging to USB 0C45:1007.
 *  - 64-byte output reports, report ID 0x04, checksum'd, request/response
 *  - 17-byte output reports, report ID 0xFF, for the 13-colour palette
 */
#define _DEFAULT_SOURCE 1
#include "omni.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct omni_dev {
    int       fd;             /* hidraw fd, -1 when closed            */
    char      node[64];
    char      name[128];
    uint16_t  vid, pid;       /* USB ID (0 if unknown)               */
    uint8_t   chunk;          /* payload bytes per packet (56)        */
    int       timeout_ms;
    uint8_t   info[34];       /* cached cmd-03 device info            */
    int       have_info;
    void    (*log_cb)(void *ctx, int is_rx, const uint8_t *frame, unsigned len);
    void     *log_ctx;
};

static char omni_err[512];

const char *omni_last_error(void) { return omni_err[0] ? omni_err : "no error"; }

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(omni_err, sizeof omni_err, fmt, ap);
    va_end(ap);
    return OMNI_ERR;
}

/* Custom per-key lighting (0xFF palette reports + cmd 0x12 table) is only
 * documented for the original 0C45:1007 firmware. On 320F:505B the HID
 * descriptor has no 0xFF report and cmd 1B returns an identity LED map
 * (00,01,02,... with FF for empty slots), not palette indices, so writing
 * palette indices with cmd 0x12 would overwrite that map. */
int omni_supports_custom_light(const omni_dev *d)
{
    return d && d->vid == OMNI_VID && d->pid == OMNI_PID;
}

static int custom_light_check(const omni_dev *d)
{
    if (omni_supports_custom_light(d))
        return OMNI_OK;
    return fail("custom per-key lighting (palette + cmd 0x12) is disabled for this "
                "keyboard (USB %04X:%04X): its LED table is not a palette-index "
                "table (see PROTOCOL.md section 13)",
                d ? d->vid : 0u, d ? d->pid : 0u);
}

/* ------------------------------------------------------------------ */
/* Device discovery (sysfs)                                            */

int omni_scan(struct omni_device_info *out, int max)
{
    const char *classdir = "/sys/class/hidraw";
    DIR *d = opendir(classdir);
    struct dirent *e;
    int found = 0;

    if (!d) {
        fail("cannot open %s: %s", classdir, strerror(errno));
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        char path[512], line[512];
        unsigned bus = 0, vid = 0, pid = 0;
        struct omni_device_info info;
        FILE *f;
        int have_id = 0;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s/device/uevent", classdir, e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;

        memset(&info, 0, sizeof info);
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3)
                have_id = 1;
            else if (strncmp(line, "HID_NAME=", 9) == 0) {
                line[strcspn(line, "\r\n")] = 0;
                snprintf(info.hid_name, sizeof info.hid_name, "%.100s", line + 9);
            }
        }
        fclose(f);

        if (!have_id ||
            !((vid == OMNI_VID  && pid == OMNI_PID) ||
              (vid == OMNI_VID2 && pid == OMNI_PID2)))
            continue;
        if (strlen(e->d_name) + 6 >= sizeof info.hidraw_node)
            continue;               /* implausible hidraw name — skip */

        snprintf(info.hidraw_node, sizeof info.hidraw_node, "/dev/%s", e->d_name);
        info.vid = (uint16_t)vid;
        info.pid = (uint16_t)pid;
        if (found < max)
            out[found] = info;
        found++;
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */

/* The configuration interface is the HID node whose report descriptor has
 * report ID 4 (the 0x04 class byte of every frame). The boot-keyboard
 * interface has none; writing a 64-byte frame to it could be taken as an
 * LED output report. Returns 1 when unsure so the probe can decide. */
static int desc_has_report_id4(const char *hidraw_node)
{
    char path[256];
    const char *base = strrchr(hidraw_node, '/');
    uint8_t desc[4096];
    FILE *f;
    size_t n, i;

    base = base ? base + 1 : hidraw_node;
    snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/report_descriptor", base);
    f = fopen(path, "rb");
    if (!f)
        return 1;
    n = fread(desc, 1, sizeof desc, f);
    fclose(f);
    for (i = 0; i < n; ) {
        uint8_t b = desc[i];
        size_t sz;
        if (b == 0xFE) {                    /* long item */
            if (i + 1 >= n) break;
            i += 3 + desc[i + 1];
            continue;
        }
        sz = (b & 3) == 3 ? 4 : (size_t)(b & 3);
        if (b == 0x85 && sz == 1 && i + 1 < n && desc[i + 1] == 0x04)
            return 1;                       /* Report ID 4 */
        i += 1 + sz;
    }
    return 0;
}

/* one probe attempt: send cmd, wait up to 400 ms for a report-ID-4 answer,
 * success when its status byte is not an error code */
static int probe_one(int fd, uint8_t cmd, uint8_t len)
{
    uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
    uint16_t sum = 0;
    struct timespec t0, t1;
    int i;

    req[0] = 0x04;
    req[3] = cmd;
    req[4] = len;
    for (i = 3; i < OMNI_REPORT_SIZE; i++) sum += req[i];
    req[1] = (uint8_t)(sum & 0xFF);
    req[2] = (uint8_t)(sum >> 8);

    if (write(fd, req, sizeof req) != (ssize_t)sizeof req)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        long left;
        ssize_t r;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        left = 400 - ((long)(t1.tv_sec - t0.tv_sec) * 1000L +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000L);
        if (left <= 0 || poll(&pf, 1, (int)left) != 1)
            return -1;
        r = read(fd, resp, sizeof resp);
        if (r < 8 || resp[0] != 0x04)
            continue;           /* other report IDs (keys, media): ignore */
        if (resp[7] == 0xFF || resp[7] == 0xFE)
            return -1;
        return 0;
    }
}

/* Is this the configuration interface? cmd 03 (read device info) is answered
 * on 320F:505B, where the old probe cmd AA returns status 0xFF; keep AA as a
 * fallback for firmware that answers it. */
static int fd_probe_aa(int fd)
{
    if (probe_one(fd, 0x03, 0x22) == 0)
        return 0;
    return probe_one(fd, 0xAA, 0);
}

omni_dev *omni_open(const char *hidraw_node)
{
    struct omni_device_info nodes[16];
    int n = omni_scan(nodes, 16), i, lim;
    omni_dev *d;

    omni_err[0] = 0;

    if (hidraw_node && hidraw_node[0])
        n = -1; /* explicit node: skip the scan list */

    d = calloc(1, sizeof *d);
    if (!d) { fail("out of memory"); return NULL; }
    d->fd = -1;
    d->chunk = OMNI_CHUNK_SIZE;
    d->timeout_ms = 1000;

    if (hidraw_node && hidraw_node[0]) {
        snprintf(d->node, sizeof d->node, "%s", hidraw_node);
        /* look up a name for it if it is in the scan list */
        struct omni_device_info all[16];
        int m = omni_scan(all, 16), k;
        for (k = 0; k < m && k < 16; k++)
            if (strcmp(all[k].hidraw_node, hidraw_node) == 0) {
                snprintf(d->name, sizeof d->name, "%s", all[k].hid_name);
                d->vid = all[k].vid;
                d->pid = all[k].pid;
            }
    } else {
        if (n <= 0) {
            fail("no DEXP OMNI keyboard found (USB %04X:%04X or %04X:%04X) — "
                 "is it plugged in? (see README, udev section)",
                 OMNI_VID, OMNI_PID, OMNI_VID2, OMNI_PID2);
            free(d);
            return NULL;
        }
        snprintf(d->node, sizeof d->node, "%s", nodes[0].hidraw_node);
        snprintf(d->name, sizeof d->name, "%s", nodes[0].hid_name);
    }

    if (hidraw_node && hidraw_node[0]) {
        d->fd = open(d->node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (d->fd < 0) {
            fail("open %s: %s (permissions? install the udev rule)", d->node, strerror(errno));
            free(d);
            return NULL;
        }
        if (fd_probe_aa(d->fd) != 0) {
            close(d->fd); d->fd = -1;
            fail("%s did not answer the 0x04/AA probe (wrong HID interface?)", d->node);
            free(d);
            return NULL;
        }
        return d;
    }

    /* auto mode: probe each candidate interface (nodes[] holds at most 16) */
    lim = n > 16 ? 16 : n;
    const char *denied = NULL;  /* first node we could not open (permissions) */
    int candidates = 0;
    for (i = 0; i < lim; i++) {
        if (!desc_has_report_id4(nodes[i].hidraw_node))
            continue;           /* e.g. the boot-keyboard interface */
        candidates++;
        int fd = open(nodes[i].hidraw_node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if ((errno == EACCES || errno == EPERM) && !denied)
                denied = nodes[i].hidraw_node;
            continue;
        }
        if (fd_probe_aa(fd) == 0) {
            d->fd = fd;
            d->vid = nodes[i].vid;
            d->pid = nodes[i].pid;
            snprintf(d->node, sizeof d->node, "%s", nodes[i].hidraw_node);
            snprintf(d->name, sizeof d->name, "%s", nodes[i].hid_name);
            return d;
        }
        close(fd);
    }
    if (denied)
        fail("permission denied opening %s — install udev/60-dexp-omni.rules "
             "(see README, udev section) and replug the keyboard, or run with sudo", denied);
    else if (!candidates)
        fail("found %d HID interface(s) of the keyboard but none has the vendor "
             "report ID 4 (configuration interface)", n);
    else
        fail("found %d HID interface(s) of the keyboard, none answered the "
             "0x04/AA probe", n);
    free(d);
    return NULL;
}

void omni_close(omni_dev *d)
{
    if (!d) return;
    if (d->fd >= 0) close(d->fd);
    free(d);
}

const char *omni_dev_node(const omni_dev *d) { return d ? d->node : ""; }
const char *omni_dev_name(const omni_dev *d) { return d ? d->name : ""; }
int omni_fd(omni_dev *d) { return d ? d->fd : -1; }

void omni_set_log(omni_dev *d, void (*cb)(void *, int, const uint8_t *, unsigned), void *ctx)
{
    if (!d) return;
    d->log_cb = cb;
    d->log_ctx = ctx;
}

void omni_set_timeout(omni_dev *d, int ms)
{
    if (d && ms > 0) d->timeout_ms = ms;
}

/* ------------------------------------------------------------------ */
/* Frame transport (PROTOCOL.md §1)                                    */

static uint16_t frame_checksum(const uint8_t *f)
{
    uint16_t s = 0;
    int i;
    for (i = 3; i < OMNI_REPORT_SIZE; i++) s = (uint16_t)(s + f[i]);
    return s;
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* send one 64-byte request, read the 64-byte answer */
static int transact(omni_dev *d, uint8_t *req, uint8_t *resp)
{
    uint16_t sum = frame_checksum(req);
    struct pollfd pf;
    ssize_t r, w;
    uint8_t scratch[OMNI_REPORT_SIZE];

    req[1] = (uint8_t)(sum & 0xFF);
    req[2] = (uint8_t)(sum >> 8);

    if (d->log_cb) d->log_cb(d->log_ctx, 0, req, OMNI_REPORT_SIZE);

    /* drain stale input */
    for (;;) {
        r = read(d->fd, scratch, sizeof scratch);
        if (r <= 0) break;
    }

    w = write(d->fd, req, OMNI_REPORT_SIZE);
    if (w != OMNI_REPORT_SIZE)
        return fail("HID write: %s", strerror(errno));

    /* Other report IDs (key presses, media keys) can arrive on this node
     * between our write and the answer: skip everything that is not a
     * report-ID-4 frame. 320F:505B echoes the request header (checksum,
     * cmd, len, offset) in bytes 1-7 even for error replies, so there the
     * command byte must match too. */
    {
        struct timespec t0, t1;
        int strict = (d->vid == OMNI_VID2 && d->pid == OMNI_PID2);
        int got = 0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!got) {
            long left;

            clock_gettime(CLOCK_MONOTONIC, &t1);
            left = d->timeout_ms - ((long)(t1.tv_sec - t0.tv_sec) * 1000L +
                                    (t1.tv_nsec - t0.tv_nsec) / 1000000L);
            if (left <= 0)
                return OMNI_ERR_NODATA;
            pf.fd = d->fd;
            pf.events = POLLIN;
            pf.revents = 0;
            if (poll(&pf, 1, (int)left) != 1)
                return OMNI_ERR_NODATA;

            memset(resp, 0, OMNI_REPORT_SIZE);
            r = read(d->fd, resp, OMNI_REPORT_SIZE);
            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                return fail("HID read: %s", strerror(errno));
            }
            if (r < 8 || resp[0] != 0x04)
                continue;
            if (strict && resp[3] != req[3])
                continue;
            got = 1;
        }
    }

    if (d->log_cb) d->log_cb(d->log_ctx, 1, resp, (unsigned)r);

    if (resp[7] == 0xFF) return OMNI_ERR_STATUS;
    if (resp[7] == 0xFE) return OMNI_ERR_STATUS2;
    return OMNI_OK;
}

/* single-shot frame with cmd/len/offset (payload already at req+8) */
static int simple_cmd(omni_dev *d, uint8_t cmd)
{
    uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
    req[0] = 0x04;
    req[3] = cmd;
    req[4] = 0;
    req[5] = 0;
    req[6] = 0;
    req[7] = 0;
    return transact(d, req, resp);
}

int omni_begin(omni_dev *d)      { return d ? simple_cmd(d, 0x01) : OMNI_ERR; }
int omni_probe(omni_dev *d)      { return d ? simple_cmd(d, 0xAA) : OMNI_ERR; }
int omni_ctrl_pulse(omni_dev *d) { return d ? simple_cmd(d, 0x1C) : OMNI_ERR; }

int omni_commit(omni_dev *d)
{
    if (!d) return OMNI_ERR;
    msleep(10);                  /* Windows app sleeps before cmd 02 */
    return simple_cmd(d, 0x02);
}

/* ------------------------------------------------------------------ */
/* Chunked transfers                                                   */

static int chunked_read(omni_dev *d, uint8_t cmd, uint8_t *dst,
                        uint16_t total, uint16_t base_off)
{
    uint8_t req[OMNI_REPORT_SIZE], resp[OMNI_REPORT_SIZE];
    uint16_t off;
    int rc;

    for (off = 0; off < total; ) {
        uint16_t chunk = (uint16_t)(d->chunk < total - off ? d->chunk : total - off);
        uint16_t addr = (uint16_t)(base_off + off);

        memset(req, 0, sizeof req);
        req[0] = 0x04;
        req[3] = cmd;
        req[4] = (uint8_t)chunk;
        req[5] = (uint8_t)(addr & 0xFF);
        req[6] = (uint8_t)(addr >> 8);
        req[7] = 0;

        if ((rc = transact(d, req, resp)) != OMNI_OK)
            return rc;
        memcpy(dst + off, resp + 8, chunk);
        off = (uint16_t)(off + chunk);
    }
    return OMNI_OK;
}

static int chunked_write(omni_dev *d, uint8_t cmd, const uint8_t *src,
                         uint16_t total, uint16_t base_off, int fixed_chunk)
{
    uint8_t req[OMNI_REPORT_SIZE], resp[OMNI_REPORT_SIZE];
    uint16_t off;
    int rc;

    for (off = 0; off < total; ) {
        uint16_t csize = (uint16_t)(fixed_chunk ? fixed_chunk : d->chunk);
        uint16_t chunk = (uint16_t)(csize < total - off ? csize : total - off);
        uint16_t addr = (uint16_t)(base_off + off);

        memset(req, 0, sizeof req);
        req[0] = 0x04;
        req[3] = cmd;
        req[4] = (uint8_t)chunk;
        req[5] = (uint8_t)(addr & 0xFF);
        req[6] = (uint8_t)(addr >> 8);
        req[7] = 0;
        memcpy(req + 8, src + off, chunk);

        if ((rc = transact(d, req, resp)) != OMNI_OK)
            return rc;
        off = (uint16_t)(off + chunk);
    }
    return OMNI_OK;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

int omni_read_block(omni_dev *d, uint16_t offset, uint8_t *buf, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_read(d, 0x05, buf, len, offset);
}

int omni_write_block(omni_dev *d, uint16_t offset, const uint8_t *buf, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_write(d, 0x06, buf, len, offset, 0);
}

int omni_read_info(omni_dev *d, uint8_t info[34])
{
    int rc;
    if (!d || d->fd < 0) return OMNI_ERR;
    rc = chunked_read(d, 0x03, info, 34, 0);
    if (rc == OMNI_OK) {
        memcpy(d->info, info, 34);
        d->have_info = 1;
    }
    return rc;
}

int omni_get_info(omni_dev *d, uint8_t info[34])
{
    if (!d) return OMNI_ERR;
    if (d->have_info) { memcpy(info, d->info, 34); return OMNI_OK; }
    return omni_read_info(d, info);
}

uint8_t omni_key_count(const omni_dev *d)
{
    return (d && d->have_info) ? d->info[5] : 0;
}

uint16_t omni_macro_capacity(const omni_dev *d)
{
    return (d && d->have_info) ? (uint16_t)(d->info[6] << 7) : 0;
}

int omni_read_keyids(omni_dev *d, uint8_t *table, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_read(d, 0x07, table, len, 0);
}

int omni_write_assignments(omni_dev *d, const uint8_t *table, uint16_t len, uint8_t bank)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    /* bank 0 -> cmd 09 (live), bank 1 -> cmd 27 (second bank) */
    return chunked_write(d, bank ? 0x27 : 0x09, table, len,
                         (uint16_t)(bank * 512), 0);
}

int omni_read_remap(omni_dev *d, uint8_t layer, uint8_t *page, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_read(d, 0x0A, page, len, (uint16_t)(layer * OMNI_REMAP_PAGE));
}

int omni_write_remap(omni_dev *d, uint8_t layer, const uint8_t *page, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_write(d, 0x0B, page, len, (uint16_t)(layer * OMNI_REMAP_PAGE), 0);
}

/* ------------------------------------------------------------------ */
/* Custom-group RGB pages (320F:505B, PROTOCOL.md 5.2b)                */

int omni_supports_group_rgb(const omni_dev *d)
{
    return d && d->vid == OMNI_VID2 && d->pid == OMNI_PID2;
}

int omni_group_off(uint8_t group, uint16_t *off)
{
    if (group > 2) return fail("custom group must be 0-2 (got %u)", group);
    if (off) *off = (uint16_t)(OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE);
    return OMNI_OK;
}

static int group_check(const omni_dev *d, uint8_t group, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    if (!omni_supports_group_rgb(d))
        return fail("custom-group RGB pages are only known on USB %04X:%04X "
                    "(this is %04X:%04X - see PROTOCOL.md 5.2b)",
                    OMNI_VID2, OMNI_PID2, d->vid, d->pid);
    if (group > 2) return fail("custom group must be 0-2 (got %u)", group);
    if (len == 0 || len > OMNI_GROUP_MAXLEN)
        return fail("group page length must be 1-%u bytes (got %u)",
                    OMNI_GROUP_MAXLEN, len);
    return OMNI_OK;
}

int omni_read_group(omni_dev *d, uint8_t group, uint8_t *page, uint16_t len)
{
    int rc = group_check(d, group, len);
    if (rc != OMNI_OK) return rc;
    return chunked_read(d, 0x05, page, len, (uint16_t)(OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE));
}

int omni_write_group(omni_dev *d, uint8_t group, const uint8_t *page,
                     uint16_t len, int via_cmd06)
{
    int rc = group_check(d, group, len);
    if (rc != OMNI_OK) return rc;
    /* Both address spaces hit the same bytes: cmd 06 at 0x6C0+group*0x200,
     * or cmd 0B at group*0x200 (which cmd 0A reads back as 0x000/0x200/0x400). */
    if (via_cmd06)
        return chunked_write(d, 0x06, page, len,
                             (uint16_t)(OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE), 0);
    return chunked_write(d, 0x0B, page, len, (uint16_t)(group * OMNI_REMAP_PAGE), 0);
}

int omni_write_group_verified(omni_dev *d, uint8_t group, const uint8_t *page,
                              uint16_t len, int via_cmd06)
{
    uint8_t back[OMNI_GROUP_MAXLEN];
    int rc;

    rc = group_check(d, group, len);
    if (rc != OMNI_OK) return rc;

    if ((rc = omni_begin(d)) != OMNI_OK) return rc;
    rc = omni_write_group(d, group, page, len, via_cmd06);
    if (rc == OMNI_OK) { msleep(10); rc = omni_commit(d); }
    else omni_commit(d);
    if (rc != OMNI_OK) return rc;

    if ((rc = omni_read_group(d, group, back, len)) != OMNI_OK) return rc;
    if (memcmp(back, page, len) != 0) {
        uint16_t i;
        for (i = 0; i < len; i++)
            if (back[i] != page[i]) break;
        return fail("group %u write did not stick: byte 0x%03X is %02X, expected %02X",
                    group, i, back[i], page[i]);
    }
    return OMNI_OK;
}

int omni_group_set_slot(uint8_t *page, uint16_t len, uint16_t slot,
                        uint8_t r, uint8_t g, uint8_t b)
{
    if (!page) return OMNI_ERR;
    if (slot >= OMNI_MAX_KEYS)
        return fail("slot %u out of range (0-%u)", slot, OMNI_MAX_KEYS - 1);
    if ((uint32_t)slot * 3 + 3 > len)
        return fail("slot %u is outside the %u-byte page read", slot, len);
    page[slot * 3 + 0] = r;
    page[slot * 3 + 1] = g;
    page[slot * 3 + 2] = b;
    return OMNI_OK;
}

int omni_group_set_key(uint8_t *page, uint16_t len, const struct omni_keymap *km,
                       int key, uint8_t r, uint8_t g, uint8_t b)
{
    int slot;
    if (!km) return OMNI_ERR;
    if (key < 0 || key >= OMNI_KEYREF_COUNT) return fail("key index %d out of range", key);
    slot = km->key_to_slot[key];
    if (slot == 0xFF) return fail("key %d has no device slot", key);
    return omni_group_set_slot(page, len, (uint16_t)slot, r, g, b);
}

void omni_group_print(FILE *fp, const uint8_t *page, uint16_t len,
                      const struct omni_keymap *km)
{
    uint16_t slot, slots = (uint16_t)(len / 3);
    unsigned lit = 0;

    for (slot = 0; slot < slots; slot++) {
        const uint8_t *c = page + slot * 3;
        int key = km ? km->slot_to_key[slot] : 0xFF;
        const char *name = (key != 0xFF && key < OMNI_KEYREF_COUNT && omni_keyref_name[key])
                           ? omni_keyref_name[key] : "?";
        if (c[0] || c[1] || c[2]) lit++;
        fprintf(fp, "slot %3u col %2u row %u  %-10s  %02X %02X %02X\n",
                slot, slot / 8, slot % 8, name, c[0], c[1], c[2]);
    }
    fprintf(fp, "%u of %u slots are non-zero\n", lit, slots);
}

int omni_read_ledidx(omni_dev *d, uint8_t *tab, uint16_t count)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_read(d, 0x1B, tab, count, 0);
}

int omni_write_ledidx(omni_dev *d, const uint8_t *tab, uint16_t count)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    if (custom_light_check(d) != OMNI_OK) return OMNI_ERR;
    return chunked_write(d, 0x12, tab, count, 0, 0);
}

int omni_write_macros(omni_dev *d, const uint8_t *image, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_write(d, 0x15, image, len, 0, 0);
}

int omni_read_small(omni_dev *d, uint8_t cmd, uint16_t offset,
                    uint8_t *buf, uint16_t len)
{
    uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
    int rc;
    if (!d || d->fd < 0) return OMNI_ERR;
    if (len > OMNI_CHUNK_SIZE) return fail("read_small: len > 56");
    req[0] = 0x04;
    req[3] = cmd;
    req[4] = (uint8_t)len;
    req[5] = (uint8_t)(offset & 0xFF);
    req[6] = (uint8_t)(offset >> 8);
    req[7] = 0;
    if ((rc = transact(d, req, resp)) != OMNI_OK)
        return rc;
    memcpy(buf, resp + 8, len);
    return OMNI_OK;
}

/* ------------------------------------------------------------------ */
/* Palette upload: 12 x report-ID-0xFF frames (PROTOCOL.md §1.1)        */

int omni_send_palette(omni_dev *d, const uint8_t rgb[OMNI_PALETTE_COUNT][3],
                      const uint8_t scale[OMNI_PALETTE_COUNT])
{
    static const uint8_t comp_first[3] = { 0xAA, 0xAB, 0xAC }; /* R G B  */
    static const uint8_t comp_second[3] = { 0xAD, 0xAE, 0xAF };
    uint8_t scaled[13];
    int c, i, half;

    if (!d || d->fd < 0) return OMNI_ERR;
    if (custom_light_check(d) != OMNI_OK) return OMNI_ERR;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < OMNI_PALETTE_COUNT; i++) {
            uint16_t v = (uint16_t)(((uint16_t)rgb[i][c] * (uint16_t)scale[i]) >> 8);
            if (v > 0xF7) v = 0xF7;              /* hardware clamp */
            scaled[i] = (uint8_t)v;
        }
        for (half = 0; half < 2; half++) {       /* 0x22 pass then 0x44 pass */
            uint8_t sub = half ? 0x44 : 0x22;
            uint8_t frames[2][17];
            int f;

            memset(frames, 0, sizeof frames);
            /* first half: 7 bytes, second half: 6 bytes */
            frames[0][0] = 0xFF; frames[0][1] = sub; frames[0][2] = comp_first[c];
            frames[0][3] = 0x00;
            memcpy(frames[0] + 4, scaled, 7);
            frames[1][0] = 0xFF; frames[1][1] = sub; frames[1][2] = comp_second[c];
            frames[1][3] = 0x00;
            memcpy(frames[1] + 4, scaled + 7, 6);

            for (f = 0; f < 2; f++) {
                if (d->log_cb) d->log_cb(d->log_ctx, 0, frames[f], 17);
                if (write(d->fd, frames[f], sizeof frames[f]) != (ssize_t)sizeof frames[f])
                    return fail("palette write: %s", strerror(errno));
                msleep(3);
            }
        }
    }
    return OMNI_OK;
}

/* ------------------------------------------------------------------ */
/* Settings record (PROTOCOL.md §4)                                    */

void omni_settings_unpack(const uint8_t r[OMNI_SETTINGS_LEN], struct omni_settings *s)
{
    memset(s, 0, sizeof *s);
    memcpy(s->raw, r, OMNI_SETTINGS_LEN);
    s->slot         = r[0];
    s->mode         = r[1];
    s->brightness   = r[2];
    s->speed        = r[3];
    s->fx           = r[4];
    s->multicolor   = r[5];
    s->blue         = r[8];
    s->green        = r[9];
    s->red          = r[10];
    s->zzcc_mode    = r[12];
    s->w_flag       = r[13];
    s->key6_flag    = r[14];
    s->win_flag     = r[15];
    s->polling      = r[16];
    s->custom_group = r[25];
    if (s->mode == 0xFE)          /* app-driven audio effects land here */
        s->mode = 29;             /* present as "Audio wave"           */
}

void omni_settings_pack(const struct omni_settings *s, uint8_t r[OMNI_SETTINGS_LEN])
{
    memcpy(r, s->raw, OMNI_SETTINGS_LEN);
    r[0]  = s->slot;
    r[1]  = (s->mode == 29 || s->mode == 30) ? 0xFE : s->mode;
    r[2]  = s->brightness;
    r[3]  = s->speed;
    r[4]  = s->fx;
    r[5]  = s->multicolor;
    if (s->layout == 1) {         /* 320F:505B: R,G,B at 6..8; 9 and 10 untouched */
        r[6] = s->red;
        r[7] = s->green;
        r[8] = s->blue;
        r[9] = s->raw[9];
        r[10] = s->raw[10];
    } else {
        r[8]  = s->blue;
        r[9]  = s->green;
        r[10] = s->red;
    }
    r[12] = s->zzcc_mode;
    r[13] = s->w_flag;
    r[14] = s->key6_flag;
    r[15] = s->win_flag;
    r[16] = s->polling;
    r[25] = s->custom_group;
}

void omni_settings_use_layout(struct omni_settings *s, int layout)
{
    s->layout = (uint8_t)layout;
    if (layout == 1) {
        s->red = s->raw[6];  s->green = s->raw[7];  s->blue = s->raw[8];
    } else {
        s->blue = s->raw[8]; s->green = s->raw[9];  s->red = s->raw[10];
    }
}

/* record layout of this keyboard: 1 for 320F:505B (colour at bytes 6-8) */
int omni_device_layout(const omni_dev *d)
{
    return d && d->vid == OMNI_VID2 && d->pid == OMNI_PID2;
}

int omni_load_settings(omni_dev *d, struct omni_settings *s)
{
    uint8_t b = 0, rec[OMNI_SETTINGS_LEN];
    int rc;

    if (!d || d->fd < 0) return OMNI_ERR;
    if ((rc = omni_read_block(d, 0, &b, 1)) != OMNI_OK)
        return rc;
    if ((rc = omni_read_block(d, (uint16_t)(b * 0x21), rec, OMNI_SETTINGS_LEN)) != OMNI_OK)
        return rc;
    omni_settings_unpack(rec, s);
    omni_settings_use_layout(s, omni_device_layout(d));
    return OMNI_OK;
}

int omni_save_settings(omni_dev *d, struct omni_settings *s)
{
    uint8_t rec[OMNI_SETTINGS_LEN];
    int rc;

    if (!d || d->fd < 0) return OMNI_ERR;
    omni_settings_pack(s, rec);
    if ((rc = omni_begin(d)) != OMNI_OK) return rc;
    rc = omni_write_block(d, (uint16_t)(s->slot * 0x21), rec, OMNI_SETTINGS_LEN);
    if (rc != OMNI_OK) { omni_commit(d); return rc; }
    return omni_commit(d);
}

int omni_apply_lighting(omni_dev *d, const struct omni_settings *s)
{
    struct omni_settings tmp = *s;
    return omni_save_settings(d, &tmp);
}

int omni_apply_custom(omni_dev *d, const uint8_t palette[OMNI_PALETTE_COUNT][3],
                      const uint8_t *led_index, uint16_t count, uint8_t group)
{
    struct omni_settings s;
    static const uint8_t scale[OMNI_PALETTE_COUNT] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
    };
    int rc;

    if (!d || d->fd < 0) return OMNI_ERR;
    if (custom_light_check(d) != OMNI_OK) return OMNI_ERR;

    /* 1. palette upload (report ID 0xFF side channel) */
    if ((rc = omni_send_palette(d, palette, scale)) != OMNI_OK)
        return rc;

    /* 2. per-LED palette-index table (cmd 12) inside a session */
    if ((rc = omni_begin(d)) != OMNI_OK) return rc;
    rc = omni_write_ledidx(d, led_index, count);
    if (rc != OMNI_OK) { omni_commit(d); return rc; }
    if ((rc = omni_commit(d)) != OMNI_OK) return rc;

    /* 3. switch the settings record to "Customize" (mode 20) */
    if ((rc = omni_load_settings(d, &s)) != OMNI_OK) return rc;
    s.mode = 20;
    s.custom_group = group;
    return omni_save_settings(d, &s);
}

int omni_query_winlock(omni_dev *d)
{
    uint8_t st[6] = {0};
    int rc = omni_read_small(d, 0x1A, 0, st, sizeof st);
    if (rc != OMNI_OK) return rc;
    return st[3] ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Key mapping & remap page building                                   */

int omni_build_keymap(const uint8_t *keyids, uint16_t keycount, struct omni_keymap *km)
{
    int key, slot;

    for (key = 0; key < OMNI_KEYREF_COUNT; key++) km->key_to_slot[key] = 0xFF;
    for (slot = 0; slot < OMNI_MAX_KEYS; slot++) km->slot_to_key[slot] = 0xFF;
    if (!keyids || keycount == 0 || keycount > OMNI_MAX_KEYS)
        return fail("bad key-ID table (count=%u)", keycount);

    for (slot = 0; slot < keycount; slot++) {
        const uint8_t *triple = keyids + slot * 3;
        for (key = 0; key < OMNI_KEYREF_COUNT; key++) {
            if (km->key_to_slot[key] != 0xFF)
                continue;
            if (omni_keyref[key][0] == triple[0] &&
                omni_keyref[key][1] == triple[1] &&
                omni_keyref[key][2] == triple[2]) {
                km->key_to_slot[key] = (uint8_t)slot;
                km->slot_to_key[slot] = (uint8_t)key;
                break;
            }
        }
    }

    /* Fallback: on the default layout slot == key index for the first
     * `keycount` keys; use identity for anything still unmatched so a
     * previously-remapped keyboard still produces a usable map. */
    for (slot = 0; slot < keycount && slot < OMNI_KEYREF_COUNT; slot++) {
        /* Only fall back to identity when key index `slot` is still free.
         * Otherwise an unmatched slot (macro M6-M8, consumer-control keys)
         * would claim the index of a real key that matched elsewhere, and
         * loading the table would overwrite that key's assignment. */
        if (km->slot_to_key[slot] == 0xFF && km->key_to_slot[slot] == 0xFF) {
            km->slot_to_key[slot] = (uint8_t)slot;
            km->key_to_slot[slot] = (uint8_t)slot;
        }
    }
    return OMNI_OK;
}

int omni_build_remap_page(const uint8_t assignment[][3], uint16_t keys,
                          const struct omni_keymap *km,
                          uint8_t page[OMNI_REMAP_PAGE], unsigned quirks)
{
    if (!assignment || !km || !page) return OMNI_ERR;
    memset(page, 0, OMNI_REMAP_PAGE);
    return omni_overlay_remap_page(assignment, keys, km, page, quirks);
}

/* Like omni_build_remap_page() but keeps whatever is already in `page` for
 * slots that have no key mapping (knob/consumer slots, M6-M8, the unused
 * 20 00 00 slots). Pass the table last read from the device as `page`. */
int omni_overlay_remap_page(const uint8_t assignment[][3], uint16_t keys,
                            const struct omni_keymap *km,
                            uint8_t page[OMNI_REMAP_PAGE], unsigned quirks)
{
    int key;
    if (!assignment || !km || !page) return OMNI_ERR;
    for (key = 0; key < keys && key < OMNI_KEYREF_COUNT; key++) {
        int slot = km->key_to_slot[key];
        if (quirks & OMNI_QUIRK_SWAP_ENTER_BACKSLASH) {
            /* model 0x14 write-path quirk (RE_cmd0A_0B_1A.md §4.3) */
            if (key == 75) slot = 107;        /* Enter    */
            if (key == 55) slot = 106;        /* Backslash*/
        }
        if (slot >= 0xFF || slot * 3 + 2 >= OMNI_REMAP_PAGE)
            continue;
        memcpy(page + slot * 3, assignment[key], 3);
    }
    return OMNI_OK;
}

/* ------------------------------------------------------------------ */
/* Assignment text helpers                                             */

static const char *usage_name(uint8_t usage)
{
    int i;
    for (i = 0; i < omni_usage_count; i++)
        if (omni_usages[i].usage == usage)
            return omni_usages[i].name;
    return NULL;
}

static const char *consumer_name(unsigned u)
{
    switch (u) {
    case 0x00B5: return "Next Track";
    case 0x00B6: return "Previous Track";
    case 0x00B7: return "Stop";
    case 0x00CD: return "Play/Pause";
    case 0x00E2: return "Mute";
    case 0x00E9: return "Volume Up";
    case 0x00EA: return "Volume Down";
    case 0x0183: return "Media Player (AL Consumer Control)";
    default:     return NULL;
    }
}

void omni_assignment_text(const uint8_t a[3], char *out, size_t n)
{
    if (!out || n == 0) return;
    if (!a) { snprintf(out, n, "—"); return; }

    if (a[0] == 0x20 && a[1] == 0 && a[2] == 0) {
        snprintf(out, n, "Disabled");
    } else if (a[0] == 0x20) {
        const char *un = usage_name(a[2]);
        char mods[64] = "";
        int i, used = 0;
        for (i = 0; i < 8; i++)
            if (a[1] & (1u << i))
                used += snprintf(mods + used, sizeof mods - used, "%s%s",
                                 used ? "+" : "", omni_mod_names[i]);
        const char *sep = mods[0] ? "+" : "";
        if (a[2] == 0)
            snprintf(out, n, "%s", mods);
        else if (un)
            snprintf(out, n, "%s%s%s", mods, sep, un);
        else
            snprintf(out, n, "%s%sUsage 0x%02X", mods, sep, a[2]);
    } else if (a[0] == 0x30) {
        /* 16-bit consumer usage: X = low byte, P = high byte (320F:505B) */
        unsigned u = (unsigned)a[1] | ((unsigned)a[2] << 8);
        const char *cn = consumer_name(u);
        if (cn) snprintf(out, n, "Consumer: %s", cn);
        else    snprintf(out, n, "Consumer 0x%04X", u);
    } else if (a[0] == 0xA0) {
        snprintf(out, n, "Media 0x%02X", a[1]);
    } else if (a[0] == 0xD0 && a[1] == 0xA2) {
        snprintf(out, n, "Macro M%u", a[2] ? a[2] : 1);
    } else if (a[0] == 0x71 || a[0] == 0x70) {
        snprintf(out, n, "Macro slot %u", a[1]);
    } else {
        snprintf(out, n, "T=%02X X=%02X P=%02X", a[0], a[1], a[2]);
    }
}

/* ---- assignment parser ------------------------------------------- */

/* lower-case and drop spaces/underscores ("Page Up" == "pageup") */
static void norm_name(const char *s, char *out, size_t n)
{
    size_t j = 0;
    for (; *s && j + 1 < n; s++) {
        if (*s == ' ' || *s == '\t' || *s == '_')
            continue;
        out[j++] = (char)tolower((unsigned char)*s);
    }
    out[j] = 0;
}

/* modifier name or alias -> bit in the modifier mask (LCtrl=0 ... RWin=7) */
static int modifier_bit(const char *norm)
{
    static const struct { const char *name; int bit; } alias[] = {
        {"lctrl",0},{"leftctrl",0},{"lcontrol",0},{"leftcontrol",0},{"ctrl",0},{"control",0},
        {"lshift",1},{"leftshift",1},{"shift",1},
        {"lalt",2},{"leftalt",2},{"alt",2},
        {"lwin",3},{"leftwin",3},{"lgui",3},{"leftgui",3},{"lmeta",3},{"lsuper",3},
        {"win",3},{"gui",3},{"meta",3},{"super",3},
        {"rctrl",4},{"rightctrl",4},{"rcontrol",4},{"rightcontrol",4},
        {"rshift",5},{"rightshift",5},
        {"ralt",6},{"rightalt",6},{"altgr",6},
        {"rwin",7},{"rightwin",7},{"rgui",7},{"rightgui",7},{"rmeta",7},{"rsuper",7},
    };
    size_t i;
    for (i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (!strcmp(norm, alias[i].name))
            return alias[i].bit;
    return -1;
}

static int usage_lookup(const char *norm)           /* usage or -1 */
{
    char nm[48];
    int i;
    for (i = 0; i < omni_usage_count; i++) {
        norm_name(omni_usages[i].name, nm, sizeof nm);
        if (!strcmp(norm, nm))
            return omni_usages[i].usage;
    }
    return -1;
}

static int keyref_lookup(const char *norm, uint8_t out[3])
{
    char nm[48];
    int i;
    for (i = 0; i < OMNI_KEYREF_COUNT; i++) {
        if (!omni_keyref_name[i])
            continue;
        norm_name(omni_keyref_name[i], nm, sizeof nm);
        if (!strcmp(norm, nm)) {
            memcpy(out, omni_keyref[i], 3);
            return 1;
        }
    }
    return 0;
}

int omni_key_index_by_name(const char *name)
{
    char norm[96], buf[128], nm[48];
    int i;

    if (!name) return -1;
    snprintf(buf, sizeof buf, "%s", name);
    norm_name(buf, norm, sizeof norm);
    if (!norm[0]) return -1;
    for (i = 0; i < OMNI_KEYREF_COUNT; i++) {
        if (!omni_keyref_name[i])
            continue;
        norm_name(omni_keyref_name[i], nm, sizeof nm);
        if (!strcmp(norm, nm))
            return i;
    }
    return -1;
}

/* exactly three hex bytes separated by space, ':' or '-' ("20 00 29") */
static int parse_three_hex(const char *s, uint8_t out[3])
{
    int cnt = 0;
    while (*s) {
        unsigned v = 0;
        int digits = 0;
        while (*s == ' ' || *s == ':' || *s == '-') s++;
        if (!*s) break;
        while (isxdigit((unsigned char)*s) && digits < 2) {
            int ch = tolower((unsigned char)*s);
            v = v * 16 + (unsigned)(ch <= '9' ? ch - '0' : ch - 'a' + 10);
            s++; digits++;
        }
        if (!digits || (*s && *s != ' ' && *s != ':' && *s != '-'))
            return 0;
        if (cnt == 3)
            return 0;
        out[cnt++] = (uint8_t)v;
    }
    return cnt == 3;
}

/* hex number with optional 0x prefix, whole string, <= maxv */
static int parse_hexval(const char *p, unsigned long maxv, unsigned long *out)
{
    char *end;
    unsigned long v;
    while (*p == ' ') p++;
    if (!*p) return 0;
    v = strtoul(p, &end, 16);
    while (*end == ' ') end++;
    if (end == p || *end || v > maxv) return 0;
    *out = v;
    return 1;
}

int omni_assignment_parse(const char *text, uint8_t out[3])
{
    char buf[128], norm[96];
    uint8_t tmp[3] = {0, 0, 0};
    unsigned long v;
    size_t i;
    int u;

    if (!text || !out) return OMNI_ERR;
    memset(out, 0, 3);
    snprintf(buf, sizeof buf, "%s", text);
    for (i = 0; buf[i]; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    norm_name(buf, norm, sizeof norm);

    if (!norm[0] || !strcmp(norm, "disabled") || !strcmp(norm, "disable") ||
        !strcmp(norm, "none") || !strcmp(norm, "off")) {
        out[0] = 0x20;
        return OMNI_OK;
    }

    /* "0x200029" — 24-bit assignment value */
    if (!strncmp(norm, "0x", 2) && parse_hexval(norm, 0xFFFFFF, &v)) {
        out[0] = (uint8_t)(v >> 16); out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)v;
        return OMNI_OK;
    }

    /* "20 00 29" / "20:00:29" */
    if (parse_three_hex(buf, tmp)) {
        memcpy(out, tmp, 3);
        return OMNI_OK;
    }

    /* "M3" / "macro M3": macro keys (this keyboard has M1-M8) */
    if (norm[0] == 'm' && norm[1] >= '1' && norm[1] <= '8' && !norm[2]) {
        out[0] = 0xD0; out[1] = 0xA2; out[2] = (uint8_t)(norm[1] - '0');
        return OMNI_OK;
    }
    if (!strncmp(norm, "macrom", 6) && norm[6] >= '1' && norm[6] <= '8' && !norm[7]) {
        out[0] = 0xD0; out[1] = 0xA2; out[2] = (uint8_t)(norm[6] - '0');
        return OMNI_OK;
    }
    /* "macro slot 7" / "macro 7": key bound to macro slot */
    {
        const char *p = NULL;
        if (!strncmp(norm, "macroslot", 9)) p = norm + 9;
        else if (!strncmp(norm, "macro", 5) && isdigit((unsigned char)norm[5])) p = norm + 5;
        if (p && *p) {
            char *end;
            unsigned long idx = strtoul(p, &end, 10);
            if (!*end && idx <= 255) {
                out[0] = 0x71; out[1] = (uint8_t)idx; out[2] = 0;
                return OMNI_OK;
            }
        }
    }
    /* "media 0x1B" (T=A0) and "consumer 0x00E9" (T=30, X=low, P=high) */
    if (!strncmp(norm, "media", 5) && parse_hexval(norm + 5, 0xFF, &v)) {
        out[0] = 0xA0; out[1] = (uint8_t)v;
        return OMNI_OK;
    }
    if (!strncmp(norm, "consumer", 8) && parse_hexval(norm + 8, 0xFFFF, &v)) {
        out[0] = 0x30; out[1] = (uint8_t)(v & 0xFF); out[2] = (uint8_t)(v >> 8);
        return OMNI_OK;
    }

    /* the whole string as a key name first, so "Num +", "Num -", "-" work */
    if ((u = usage_lookup(norm)) >= 0) {
        out[0] = 0x20; out[2] = (uint8_t)u;
        return OMNI_OK;
    }
    if (keyref_lookup(norm, tmp)) {
        memcpy(out, tmp, 3);
        return OMNI_OK;
    }

    /* "ctrl+shift+a", "left shift", "alt+f4": modifiers plus at most one key */
    {
        unsigned mask = 0;
        int usage = -1;
        const char *tok = buf;

        for (;;) {
            char piece[64], t[64];
            const char *plus = strchr(tok, '+');
            size_t len = plus ? (size_t)(plus - tok) : strlen(tok);
            int b, k;

            if (len >= sizeof piece) len = sizeof piece - 1;
            memcpy(piece, tok, len);
            piece[len] = 0;
            norm_name(piece, t, sizeof t);

            if (!t[0])
                break;                                   /* empty token */
            if ((b = modifier_bit(t)) >= 0) {
                mask |= 1u << b;
            } else if ((k = usage_lookup(t)) >= 0) {
                if (usage >= 0)
                    return fail("cannot parse assignment \"%s\": more than one non-modifier key", text);
                usage = k;
            } else {
                return fail("cannot parse assignment \"%s\": unknown key \"%s\"", text, piece);
            }
            if (!plus)
                goto combo_done;
            tok = plus + 1;
        }
        return fail("cannot parse assignment \"%s\"", text);
combo_done:
        out[0] = 0x20;
        out[1] = (uint8_t)mask;
        out[2] = (uint8_t)(usage < 0 ? 0 : usage);
        return OMNI_OK;
    }
}


/* ---- small exports used by omni_profile.c ---------------------------- */
int omni_fail(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return fail("%s", tmp);
}

void omni_device_id(const omni_dev *d, uint16_t *vid, uint16_t *pid)
{
    *vid = d ? d->vid : 0;
    *pid = d ? d->pid : 0;
}

int omni_read_cmd(omni_dev *d, uint8_t cmd, uint16_t offset, uint8_t *buf, uint16_t len)
{
    if (!d || d->fd < 0) return OMNI_ERR;
    return chunked_read(d, cmd, buf, len, offset);
}
