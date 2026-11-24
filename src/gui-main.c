/*
 * gui-main.c — omni-gtk: main window, device page, protocol console
 *
 * GTK3 configurator for the DEXP OMNI keyboard, reverse-engineered from
 * the Windows OMNI.exe configurator (see PROTOCOL.md).
 */
#include "gui.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */

void app_log(App *a, const char *fmt, ...)
{
    va_list ap;
    char ts[32], line[1024], buf[1100];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (!a || !a->logbuf) return;
    strftime(ts, sizeof ts, "%H:%M:%S", tm);
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(a->logbuf, &iter);
    snprintf(buf, sizeof buf, "%s  %s\n", ts, line);
    gtk_text_buffer_insert(a->logbuf, &iter, buf, -1);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(a->log_view),
        gtk_text_buffer_get_insert(a->logbuf));
}

void app_status(App *a, const char *fmt, ...)
{
    va_list ap;
    char line[512];
    if (!a || !a->status) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    guint ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(a->status), "main");
    gtk_statusbar_pop(GTK_STATUSBAR(a->status), ctx);
    gtk_statusbar_push(GTK_STATUSBAR(a->status), ctx, line);
}

int app_dev_check(App *a)
{
    if (!a->have_dev || !a->dev) {
        app_status(a, "No device — open the Device page and connect first.");
        return 0;
    }
    return 1;
}

int app_key_to_slot(App *a, int key)
{
    if (!a->have_keymap || key < 0 || key >= OMNI_KEYREF_COUNT) return -1;
    uint8_t s = a->km.key_to_slot[key];
    return s == 0xFF ? -1 : s;
}

int app_slot_to_key(App *a, int slot)
{
    if (!a->have_keymap || slot < 0 || slot >= OMNI_MAX_KEYS) return -1;
    uint8_t k = a->km.slot_to_key[slot];
    return k == 0xFF ? -1 : k;
}

int hex_parse(const char *s, uint8_t *out, int maxlen)
{
    int n = 0;
    unsigned v;
    while (*s && n < maxlen) {
        while (*s == ' ' || *s == ':' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v;
        while (*s && *s != ' ' && *s != ':' && *s != ',' && *s != '\t') s++;
    }
    return n;
}

void hex_format(const uint8_t *b, unsigned n, char *out, size_t cap)
{
    size_t used = 0;
    for (unsigned i = 0; i < n && used + 4 < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%02x ", b[i]);
}

static void log_frame_cb(void *ctx, int is_rx, const uint8_t *frame, unsigned len)
{
    App *a = ctx;
    char hex[3 * 64 + 1];
    hex_format(frame, len > 64 ? 64 : len, hex, sizeof hex);
    app_log(a, "[%s] %s", is_rx ? "RX" : "TX", hex);
}

/* ------------------------------------------------------------------ */
/* Connect / disconnect                                                */

int app_connect(App *a, const char *node)
{
    omni_dev *d = omni_open(node && node[0] ? node : NULL);
    uint16_t kc;
    int rc;

    if (!d) {
        app_status(a, "Connect failed: %s", omni_last_error());
        app_log(a, "connect: FAILED — %s", omni_last_error());
        return 0;
    }
    a->dev = d;
    a->have_dev = 1;
    omni_set_log(d, log_frame_cb, a);

    /* init sequence — mirrors OMNI.exe 0x495040 (PROTOCOL.md §8) */
    rc = omni_begin(d);
    if (rc == OMNI_OK) rc = omni_read_info(d, a->info);
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;
    if (rc == OMNI_OK) rc = omni_read_keyids(d, a->keyids, (uint16_t)(kc * 3));
    if (rc == OMNI_OK) rc = omni_read_ledidx(d, a->ledidx, kc);
    if (rc == OMNI_OK) rc = omni_commit(d);

    if (rc != OMNI_OK) {
        app_log(a, "init sequence failed: %d (%s)", rc, omni_last_error());
        app_status(a, "Connected, but init failed: %s", omni_last_error());
    }

    if (omni_build_keymap(a->keyids, kc, &a->km) == OMNI_OK)
        a->have_keymap = 1;

    /* live assignments = cmd-07 table (slot-major) */
    for (int s = 0; s < kc; s++) {
        int k = app_slot_to_key(a, s);
        if (k >= 0)
            memcpy(a->assignment[k], a->keyids + s * 3, 3);
    }
    a->have_ledidx = 1;

    if (omni_load_settings(d, &a->settings) == OMNI_OK)
        a->have_settings = 1;

    a->quirks = (a->info[0] == 0x14) ? OMNI_QUIRK_SWAP_ENTER_BACKSLASH : 0;

    app_log(a, "connected: %s (%s), model %u, %u keys, macro storage %u B",
            omni_dev_node(d), omni_dev_name(d), a->info[0], a->info[5],
            omni_macro_capacity(d));
    if (a->quirks)
        app_log(a, "model 0x14: Enter/Backslash slot-swap quirk enabled on writes");

    app_refresh_all(a);
    app_status(a, "Connected: %s", omni_dev_name(d));
    return 1;
}

void app_disconnect(App *a)
{
    if (a->dev) omni_close(a->dev);
    a->dev = NULL;
    a->have_dev = 0;
    a->have_keymap = 0;
    a->have_settings = 0;
    a->have_ledidx = 0;
    app_refresh_all(a);
    app_status(a, "Disconnected.");
    app_log(a, "disconnected");
}

void app_refresh_all(App *a)
{
    char buf[1024];
    a->in_refresh = 1;

    /* device page info */
    if (a->have_dev) {
        snprintf(buf, sizeof buf,
            "<b>%s</b>\n"
            "node: %s   model id: %u (0x%02X)\n"
            "keys/slots: %u    macro storage: %u bytes\n"
            "active settings slot: %u    win-lock flag: %u    key6: %u",
            omni_dev_name(a->dev), omni_dev_node(a->dev),
            a->info[0], a->info[0], a->info[5], omni_macro_capacity(a->dev),
            a->settings.slot, a->settings.win_flag, a->settings.key6_flag);
        gtk_label_set_markup(GTK_LABEL(a->dev_info_lbl), buf);
        gtk_label_set_text(GTK_LABEL(a->conn_lbl), "connected");
    } else {
        gtk_label_set_markup(GTK_LABEL(a->dev_info_lbl),
            "<i>not connected</i>");
        gtk_label_set_text(GTK_LABEL(a->conn_lbl), "disconnected");
    }

    /* lighting page */
    if (a->have_settings) {
        {
            uint8_t ids[32];
            int n = omni_selectable_modes(ids), pos = -1, i;
            for (i = 0; i < n; i++)
                if (ids[i] == a->settings.mode) pos = i;
            gtk_combo_box_set_active(GTK_COMBO_BOX(a->mode_combo), pos);
        }
        gtk_range_set_value(GTK_RANGE(a->bright_scale), a->settings.brightness);
        gtk_range_set_value(GTK_RANGE(a->speed_scale), a->settings.speed);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->fx_spin), a->settings.fx);
        GdkRGBA rgba = {
            a->settings.red / 255.0, a->settings.green / 255.0,
            a->settings.blue / 255.0, 1.0 };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(a->color_btn), &rgba);
        gtk_switch_set_active(GTK_SWITCH(a->multi_sw), a->settings.multicolor);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->group_combo),
                                 a->settings.custom_group < 3 ? a->settings.custom_group : 0);
    }

    /* device page switches */
    if (a->have_settings) {
        gtk_switch_set_active(GTK_SWITCH(a->key6_sw), a->settings.key6_flag);
        gtk_switch_set_active(GTK_SWITCH(a->win_sw), a->settings.win_flag);
        gtk_switch_set_active(GTK_SWITCH(a->wflag_sw), a->settings.w_flag);
        switch (a->settings.polling) {
        case 1: gtk_combo_box_set_active(GTK_COMBO_BOX(a->poll_combo), 0); break;
        case 2: gtk_combo_box_set_active(GTK_COMBO_BOX(a->poll_combo), 1); break;
        case 4: gtk_combo_box_set_active(GTK_COMBO_BOX(a->poll_combo), 2); break;
        case 8: gtk_combo_box_set_active(GTK_COMBO_BOX(a->poll_combo), 3); break;
        default: gtk_combo_box_set_active(GTK_COMBO_BOX(a->poll_combo), -1); break;
        }
    }

    kb_view_refresh(a->kb_select);
    if (a->kb_paint) kb_view_refresh(a->kb_paint);
    macro_refresh(a);
    if (a->macro_cap_lbl) {
        if (a->have_dev) {
            char mbuf[256];
            snprintf(mbuf, sizeof mbuf,
                "<b>On-board macro storage: %u bytes</b> "
                "(device reports capacity in 128-byte units)",
                omni_macro_capacity(a->dev));
            gtk_label_set_markup(GTK_LABEL(a->macro_cap_lbl), mbuf);
        } else {
            gtk_label_set_markup(GTK_LABEL(a->macro_cap_lbl),
                "<i>not connected — connect on the Device page</i>");
        }
    }
    a->in_refresh = 0;
}

/* ------------------------------------------------------------------ */
/* Device page                                                         */

static void on_scan(App *a, GtkWidget *btn)
{
    struct omni_device_info list[16];
    int n = omni_scan(list, 16), i;
    (void)btn;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->node_combo));
    if (n <= 0) {
        app_status(a, "No DEXP OMNI (0C45:1007 / 320F:505B) interfaces found. "
                      "Is the keyboard plugged in? (udev rule installed?)");
        return;
    }
    for (i = 0; i < n; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->node_combo),
                                       list[i].hidraw_node);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->node_combo), 0);
    app_status(a, "Found %d HID interface(s) — the connector probes each "
                  "with cmd AA and picks the config interface.", n);
}

static void on_connect(App *a, GtkWidget *btn)
{
    (void)btn;
    const gchar *node = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(a->node_combo));
    app_connect(a, node);
}

static void on_disconnect(GtkWidget *btn, App *a)
{
    (void)btn;
    app_disconnect(a);
}

/* apply one settings byte via load-modify-save */
static int settings_apply_byte(App *a, const char *field,
                               void (*set)(struct omni_settings *))
{
    struct omni_settings s;
    int rc;
    if (!app_dev_check(a)) return 0;
    rc = omni_load_settings(a->dev, &s);
    if (rc != OMNI_OK) {
        app_status(a, "load settings failed: %s", omni_last_error());
        return 0;
    }
    set(&s);
    rc = omni_save_settings(a->dev, &s);
    if (rc != OMNI_OK) {
        app_status(a, "%s: write failed: %s", field, omni_last_error());
        return 0;
    }
    a->settings = s;
    a->have_settings = 1;
    app_log(a, "settings: %s applied", field);
    app_status(a, "%s applied.", field);
    return 1;
}

static void set_key6(struct omni_settings *s) { s->key6_flag = 1; }
static void clr_key6(struct omni_settings *s) { s->key6_flag = 0; }
static void set_win(struct omni_settings *s)  { s->win_flag = 1; }
static void clr_win(struct omni_settings *s)  { s->win_flag = 0; }
static void set_wflag(struct omni_settings *s){ s->w_flag = 1; }
static void clr_wflag(struct omni_settings *s){ s->w_flag = 0; }

static void on_key6(GtkSwitch *sw, GParamSpec *ps, App *a)
{
    if (a->in_refresh || !a->have_dev) return;
    (void)ps;
    gboolean on = gtk_switch_get_active(sw);
    settings_apply_byte(a, "6KRO/Key6 flag", on ? set_key6 : clr_key6);
}

static void on_winlock(GtkSwitch *sw, GParamSpec *ps, App *a)
{
    if (a->in_refresh || !a->have_dev) return;
    (void)ps;
    gboolean on = gtk_switch_get_active(sw);
    settings_apply_byte(a, "Windows-key lock", on ? set_win : clr_win);
}

static void on_wflag(GtkSwitch *sw, GParamSpec *ps, App *a)
{
    if (a->in_refresh || !a->have_dev) return;
    (void)ps;
    gboolean on = gtk_switch_get_active(sw);
    settings_apply_byte(a, "W flag", on ? set_wflag : clr_wflag);
}

static void on_poll_apply(App *a, GtkWidget *btn)
{
    struct omni_settings s;
    int rc, idx;
    int ms[4] = { 1, 2, 4, 8 };
    (void)btn;
    if (!app_dev_check(a)) return;
    idx = gtk_combo_box_get_active(GTK_COMBO_BOX(a->poll_combo));
    if (idx < 0) { app_status(a, "Pick a polling interval first."); return; }
    rc = omni_load_settings(a->dev, &s);
    if (rc != OMNI_OK) { app_status(a, "load failed: %s", omni_last_error()); return; }
    s.polling = (uint8_t)ms[idx];
    rc = omni_save_settings(a->dev, &s);
    if (rc != OMNI_OK) { app_status(a, "write failed: %s", omni_last_error()); return; }
    a->settings = s;
    app_status(a, "Polling rate set to %d ms (%d Hz).", ms[idx], 1000 / ms[idx]);
    app_log(a, "polling rate -> %d ms", ms[idx]);
}

static void on_query_winlock(App *a, GtkWidget *btn)
{
    (void)btn;
    if (!app_dev_check(a)) return;
    int v = omni_query_winlock(a->dev);
    if (v < 0)
        app_status(a, "win-lock query failed: %s", omni_last_error());
    else
        app_status(a, "Windows key is %s (cmd 1A).", v ? "LOCKED" : "unlocked");
}

static void on_probe(App *a, GtkWidget *btn)
{
    (void)btn;
    if (!app_dev_check(a)) return;
    int rc = omni_probe(a->dev);
    app_status(a, rc == OMNI_OK ? "Probe (cmd AA): OK" :
                       "Probe failed: %s", rc == OMNI_OK ? "" : omni_last_error());
}

static void on_pulse(App *a, GtkWidget *btn)
{
    (void)btn;
    if (!app_dev_check(a)) return;
    int rc = omni_ctrl_pulse(a->dev);
    app_status(a, rc == OMNI_OK ? "Control pulse (cmd 1C) sent." :
                       "Pulse failed: %s", rc == OMNI_OK ? "" : omni_last_error());
}

GtkWidget *build_device_page(App *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame, *hbox, *btn, *lbl;

    /* -- connection -- */
    frame = gtk_frame_new("Connection");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), hbox);

    btn = gtk_button_new_with_label("Scan");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_scan), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    a->node_combo = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(hbox), a->node_combo, TRUE, TRUE, 0);

    btn = gtk_button_new_with_label("Connect");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_connect), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Disconnect");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_disconnect), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    a->conn_lbl = gtk_label_new("disconnected");
    gtk_box_pack_start(GTK_BOX(hbox), a->conn_lbl, FALSE, FALSE, 6);

    /* -- info -- */
    frame = gtk_frame_new("Device (cmd 03 / cmd 07 / cmd 05)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    a->dev_info_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(a->dev_info_lbl), 0);
    gtk_widget_set_margin_start(a->dev_info_lbl, 8);
    gtk_widget_set_margin_end(a->dev_info_lbl, 8);
    gtk_widget_set_margin_top(a->dev_info_lbl, 8);
    gtk_widget_set_margin_bottom(a->dev_info_lbl, 8);
    gtk_container_add(GTK_CONTAINER(frame), a->dev_info_lbl);

    /* -- system settings -- */
    frame = gtk_frame_new("System settings (settings record, PROTOCOL.md §4)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    lbl = gtk_label_new("Polling rate:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    a->poll_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->poll_combo), "1 ms (1000 Hz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->poll_combo), "2 ms (500 Hz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->poll_combo), "4 ms (250 Hz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->poll_combo), "8 ms (125 Hz)");
    gtk_box_pack_start(GTK_BOX(hbox), a->poll_combo, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Apply");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_poll_apply), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    struct { const char *name; GtkWidget **sw; GCallback cb; const char *tip; } sws[] = {
        { "6KRO / Key6 flag", &a->key6_sw, G_CALLBACK(on_key6),
          "Settings record byte 14 (Key6Flag)" },
        { "Windows-key lock", &a->win_sw, G_CALLBACK(on_winlock),
          "Settings record byte 15 (WinFlag)" },
        { "W flag", &a->wflag_sw, G_CALLBACK(on_wflag),
          "Settings record byte 13 (WFlag)" },
    };
    for (unsigned i = 0; i < 3; i++) {
        GtkWidget *h2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        lbl = gtk_label_new(sws[i].name);
        gtk_box_pack_start(GTK_BOX(h2), lbl, FALSE, FALSE, 0);
        *sws[i].sw = gtk_switch_new();
        gtk_widget_set_tooltip_text(*sws[i].sw, sws[i].tip);
        g_signal_connect(*sws[i].sw, "notify::active", sws[i].cb, a);
        gtk_box_pack_start(GTK_BOX(h2), *sws[i].sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), h2, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    btn = gtk_button_new_with_label("Query win-lock (cmd 1A)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_query_winlock), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Probe (cmd AA)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_probe), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Control pulse (cmd 1C)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_pulse), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "<i>Switches take effect immediately (load-modify-save of the active "
        "settings slot).</i>");
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
    return box;
}

/* ------------------------------------------------------------------ */
/* Protocol console page                                               */

/* (widget bundle built below; handlers first) */
typedef struct ConsoleWidgets {
    App *app;
    GtkWidget *cmd_spin, *len_spin, *off_spin, *payload_entry, *raw_entry;
} ConsoleWidgets;

static void on_send_cmd_frame(GtkWidget *btn, ConsoleWidgets *cw)
{
    App *a = cw->app;
    (void)btn;
    if (!app_dev_check(a)) return;

    uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
    uint8_t payload[OMNI_CHUNK_SIZE];
    int cmd  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cw->cmd_spin));
    int len  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cw->len_spin));
    int off  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cw->off_spin));
    const char *hex = gtk_entry_get_text(GTK_ENTRY(cw->payload_entry));
    int n = hex_parse(hex, payload, OMNI_CHUNK_SIZE);

    if (n < 0) { app_status(a, "Bad payload hex."); return; }
    if (n < len) len = n;
    if (len < 0) len = 0;

    req[0] = 0x04;
    req[3] = (uint8_t)cmd;
    req[4] = (uint8_t)len;
    req[5] = (uint8_t)(off & 0xFF);
    req[6] = (uint8_t)(off >> 8);
    req[7] = 0;
    memcpy(req + 8, payload, (size_t)len);

    /* reuse the library transport via a tiny trick: build + send manually */
    uint16_t sum = 0;
    for (int i = 3; i < OMNI_REPORT_SIZE; i++) sum += req[i];
    req[1] = (uint8_t)(sum & 0xFF);
    req[2] = (uint8_t)(sum >> 8);

    char out[3 * 64 + 1];
    hex_format(req, OMNI_REPORT_SIZE, out, sizeof out);
    app_log(a, "[TX*] %s", out);

    ssize_t w = write(omni_fd(a->dev), req, OMNI_REPORT_SIZE);
    if (w != OMNI_REPORT_SIZE) {
        app_status(a, "write failed: %s", strerror(errno));
        return;
    }
    struct pollfd pf = { omni_fd(a->dev), POLLIN, 0 };
    if (poll(&pf, 1, 1000) != 1) { app_status(a, "no response (timeout)"); return; }
    ssize_t r = read(omni_fd(a->dev), resp, OMNI_REPORT_SIZE);
    if (r < 8) { app_status(a, "short response"); return; }
    hex_format(resp, (unsigned)r, out, sizeof out);
    app_log(a, "[RX*] %s", out);
    app_log(a, "status byte recv[7] = %02x (%s)", resp[7],
            resp[7] == 0xFF ? "error -102" : resp[7] == 0xFE ? "error -103" : "OK");
    app_status(a, "Command 0x%02X sent — see protocol log.", cmd);
}

static void on_send_raw(GtkWidget *btn, ConsoleWidgets *cw)
{
    App *a = cw->app;
    (void)btn;
    if (!app_dev_check(a)) return;

    uint8_t req[OMNI_REPORT_SIZE] = {0}, resp[OMNI_REPORT_SIZE];
    const char *hex = gtk_entry_get_text(GTK_ENTRY(cw->raw_entry));
    int n = hex_parse(hex, req, OMNI_REPORT_SIZE);
    if (n <= 0) { app_status(a, "Bad frame hex."); return; }

    char out[3 * 64 + 1];
    hex_format(req, OMNI_REPORT_SIZE, out, sizeof out);
    app_log(a, "[TX raw] %s", out);

    if (write(omni_fd(a->dev), req, OMNI_REPORT_SIZE) != OMNI_REPORT_SIZE) {
        app_status(a, "write failed: %s", strerror(errno));
        return;
    }
    struct pollfd pf = { omni_fd(a->dev), POLLIN, 0 };
    if (poll(&pf, 1, 1000) != 1) { app_status(a, "no response (timeout)"); return; }
    ssize_t r = read(omni_fd(a->dev), resp, OMNI_REPORT_SIZE);
    if (r < 8) { app_status(a, "short response"); return; }
    hex_format(resp, (unsigned)r, out, sizeof out);
    app_log(a, "[RX raw] %s", out);
    app_status(a, "Raw frame sent.");
}

static void on_log_clear(GtkWidget *btn, App *a)
{
    (void)btn;
    gtk_text_buffer_set_text(a->logbuf, "", -1);
}

static void on_log_save(GtkWidget *btn, App *a)
{
    (void)btn;
    GtkTextIter start, end;
    char *text;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save protocol log",
        GTK_WINDOW(a->win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_text_buffer_get_bounds(a->logbuf, &start, &end);
        text = gtk_text_buffer_get_text(a->logbuf, &start, &end, FALSE);
        g_file_set_contents(fn, text, (gssize)strlen(text), NULL);
        g_free(text);
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

GtkWidget *build_console_page(App *a)
{
    static ConsoleWidgets cw;   /* static: widgets outlive the builder */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame, *hbox, *lbl;

    cw.app = a;

    frame = gtk_frame_new("Send a 0x04-class frame (see PROTOCOL.md §1)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_container_set_border_width(GTK_CONTAINER(g), 8);
    gtk_container_add(GTK_CONTAINER(frame), g);

    lbl = gtk_label_new("Command byte:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    cw.cmd_spin = gtk_spin_button_new_with_range(0, 255, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw.cmd_spin), 0x05);
    gtk_grid_attach(GTK_GRID(g), lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), cw.cmd_spin, 1, 0, 1, 1);

    lbl = gtk_label_new("Payload length:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    cw.len_spin = gtk_spin_button_new_with_range(0, 56, 1);
    gtk_grid_attach(GTK_GRID(g), lbl, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), cw.len_spin, 3, 0, 1, 1);

    lbl = gtk_label_new("Offset:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    cw.off_spin = gtk_spin_button_new_with_range(0, 65535, 1);
    gtk_grid_attach(GTK_GRID(g), lbl, 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), cw.off_spin, 5, 0, 1, 1);

    lbl = gtk_label_new("Payload hex:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    cw.payload_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cw.payload_entry), "e.g. 00 1c 00 1c");
    gtk_widget_set_hexpand(cw.payload_entry, TRUE);
    gtk_grid_attach(GTK_GRID(g), lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), cw.payload_entry, 1, 1, 5, 1);

    GtkWidget *btn = gtk_button_new_with_label("Send (checksum computed automatically)");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_send_cmd_frame), &cw);
    gtk_grid_attach(GTK_GRID(g), btn, 1, 2, 3, 1);

    frame = gtk_frame_new("Send raw 64-byte frame (checksum NOT computed)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), hbox);
    cw.raw_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cw.raw_entry),
        "64 hex bytes, e.g. 04 00 00 1c 00 00 00 00 00 ...");
    gtk_box_pack_start(GTK_BOX(hbox), cw.raw_entry, TRUE, TRUE, 0);
    btn = gtk_button_new_with_label("Send raw");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_send_raw), &cw);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    frame = gtk_frame_new("Protocol log (all library traffic is mirrored here)");
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(frame), vbox);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
    a->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->log_view), GTK_WRAP_WORD_CHAR);
    a->logbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log_view));
    gtk_container_add(GTK_CONTAINER(scroll), a->log_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    btn = gtk_button_new_with_label("Clear");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_log_clear), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Save to file…");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_log_save), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
    return box;
}

/* ------------------------------------------------------------------ */
/* main                                                                */

static const char *shot_path = NULL;
static int shot_page = -1;

static gboolean screenshot_cb(gpointer data)
{
    App *a = data;
    GdkWindow *win = gtk_widget_get_window(a->win);
    if (win && shot_path) {
        int w = gdk_window_get_width(win), h = gdk_window_get_height(win);
        GdkPixbuf *pb = gdk_pixbuf_get_from_window(win, 0, 0, w, h);
        if (pb) {
            gdk_pixbuf_save(pb, shot_path, "png", NULL, NULL);
            g_object_unref(pb);
            g_printerr("screenshot saved: %s\n", shot_path);
        }
    }
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    App app;
    GtkWidget *vbox;

    memset(&app, 0, sizeof app);
    app.selected_key = -1;
    app.paint_idx = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--page") && i + 1 < argc)
            shot_page = atoi(argv[++i]);
    }

    gtk_init(&argc, &argv);

    /* default palette: 13 distinguishable colours (before pages are built) */
    {
        static const uint8_t def[13][3] = {
            {255,0,0},{255,85,0},{255,170,0},{255,255,0},{170,255,0},{85,255,0},
            {0,255,0},{0,255,127},{0,255,255},{0,127,255},{127,0,255},{255,0,255},{255,255,255}
        };
        memcpy(app.palette, def, sizeof def);
    }

    app.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.win),
        "omni-gtk — DEXP OMNI keyboard configurator (reverse-engineered)");
    gtk_window_set_default_size(GTK_WINDOW(app.win), 980, 720);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.win), vbox);

    app.nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), app.nb, TRUE, TRUE, 0);

    GtkWidget *page;
    page = build_device_page(&app);
    gtk_notebook_append_page(GTK_NOTEBOOK(app.nb), page, gtk_label_new("Device"));
    page = build_light_page(&app);
    gtk_notebook_append_page(GTK_NOTEBOOK(app.nb), page, gtk_label_new("Lighting"));
    page = build_keys_page(&app);
    gtk_notebook_append_page(GTK_NOTEBOOK(app.nb), page, gtk_label_new("Keys"));
    page = build_macro_page(&app);
    gtk_notebook_append_page(GTK_NOTEBOOK(app.nb), page, gtk_label_new("Macros"));
    page = build_console_page(&app);
    gtk_notebook_append_page(GTK_NOTEBOOK(app.nb), page, gtk_label_new("Protocol console"));

    app.status = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), app.status, FALSE, FALSE, 0);

    g_signal_connect(app.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    app_refresh_all(&app);
    app_status(&app, "Connect the keyboard on the Device page. "
                     "Protocol reference: PROTOCOL.md in the source tree.");

    gtk_widget_show_all(app.win);

    if (shot_path) {
        if (shot_page >= 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(app.nb), shot_page);
        while (gtk_events_pending()) gtk_main_iteration();
        g_timeout_add(700, screenshot_cb, &app);
    }
    gtk_main();
    return 0;
}
