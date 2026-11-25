/*
 * gui-keys.c — keyboard view widget + key remapping page
 *
 * The keyboard geometry is the exact geometry of the original Windows
 * configurator skin (omni_layout.c). Clicking a key selects it; the
 * assignment editor builds the 3-byte [T,X,P] record documented in
 * PROTOCOL.md §6.
 */
#include "gui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* logical canvas: skin 673x218 + 4px margins + 22px strip for hidden keys */
#define KV_MARGIN   4.0
#define KV_STRIP_Y  226.0
#define KV_STRIP_H  22.0
#define KV_STRIP_SW 30.0
#define KV_STRIP_SH 18.0
#define KV_STRIP_GAP 4.0
#define KV_LOGICAL_W (OMNI_KB_CANVAS_W + 2 * KV_MARGIN)
#define KV_LOGICAL_H (KV_STRIP_Y + KV_STRIP_H + KV_MARGIN)

typedef struct KbView {
    App *app;
    int  paint_mode;
} KbView;

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static void set_color(cairo_t *cr, const uint8_t rgb[3], double dim)
{
    cairo_set_source_rgb(cr, rgb[0] / 255.0 * dim, rgb[1] / 255.0 * dim, rgb[2] / 255.0 * dim);
}

static const char *key_name(int key)
{
    if (key < 0 || key >= OMNI_KEYREF_COUNT) return "?";
    return omni_keyref_name[key] ? omni_keyref_name[key] : "?";
}

static void draw_key(cairo_t *cr, double x, double y, double w, double h,
                     const char *label, gboolean selected, gboolean small)
{
    cairo_set_line_width(cr, selected ? 2.5 : 1.0);
    if (selected)
        cairo_set_source_rgb(cr, 0.10, 0.37, 0.85);
    else
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    rounded_rect(cr, x, y, w, h, 3);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, selected ? 0.80 : 0.95, selected ? 0.88 : 0.95,
                               selected ? 1.00 : 0.95);
    cairo_fill(cr);

    if (label && w > 13) {
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
        cairo_set_font_size(cr, small ? 7.5 : 8.5);
        cairo_text_extents(cr, label, &ext);
        cairo_move_to(cr, x + (w - ext.width) / 2 - ext.x_bearing,
                           y + (h - ext.height) / 2 - ext.y_bearing);
        cairo_show_text(cr, label);
    }
}

static gboolean kb_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    KbView *kv = data;
    App *a = kv->app;
    GtkAllocation alloc;
    double sx, sy;
    char buf[64];

    gtk_widget_get_allocation(w, &alloc);
    sx = alloc.width / KV_LOGICAL_W;
    sy = alloc.height / KV_LOGICAL_H;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    /* main layout */
    for (int i = 0; i < omni_key_box_count; i++) {
        const struct omni_key_box *b = &omni_key_boxes[i];
        double x = (KV_MARGIN + b->x) * sx, y = (KV_MARGIN + b->y) * sy;
        double wd = b->w * sx, ht = b->h * sy;

        if (kv->paint_mode && a->paint_raw) {
            /* 320F:505B: the page holds one raw RGB triple per slot */
            int slot = app_key_to_slot(a, b->key_idx);
            const uint8_t *c = NULL;
            if (slot >= 0 && a->have_group && (uint32_t)slot * 3 + 3 <= OMNI_GROUP_MAXLEN)
                c = a->group_rgb + slot * 3;
            cairo_set_line_width(cr, 1.0);
            if (c && (c[0] || c[1] || c[2])) {
                set_color(cr, c, 1.0);
                rounded_rect(cr, x, y, wd, ht, 3);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.38);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
                rounded_rect(cr, x, y, wd, ht, 3);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.38);
                cairo_stroke(cr);
            }
            if (wd > 13) {
                cairo_text_extents_t ext;
                /* dark label on bright keys, white on dark ones (BT.601 luma) */
                if (c) {
                    unsigned luma = (c[0] * 299u + c[1] * 587u + c[2] * 114u) / 1000u;
                    if (luma > 140u)
                        cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.95);
                    else
                        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
                } else {
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
                }
                cairo_set_font_size(cr, 8.5);
                cairo_text_extents(cr, key_name(b->key_idx), &ext);
                cairo_move_to(cr, x + (wd - ext.width) / 2 - ext.x_bearing,
                                   y + (ht - ext.height) / 2 - ext.y_bearing);
                cairo_show_text(cr, key_name(b->key_idx));
            }
        } else if (kv->paint_mode) {
            int slot = app_key_to_slot(a, b->key_idx);
            uint8_t idx = (slot >= 0 && a->have_ledidx) ? a->ledidx[slot] : 0xFF;
            cairo_set_line_width(cr, 1.0);
            if (idx <= OMNI_PALETTE_COUNT - 1) {
                set_color(cr, a->palette[idx], 1.0);
                rounded_rect(cr, x, y, wd, ht, 3);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.38);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
                rounded_rect(cr, x, y, wd, ht, 3);
                cairo_fill_preserve(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.38);
                cairo_stroke(cr);
            }
            if (wd > 13) {
                cairo_text_extents_t ext;
                cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
                cairo_set_font_size(cr, 8.5);
                cairo_text_extents(cr, key_name(b->key_idx), &ext);
                cairo_move_to(cr, x + (wd - ext.width) / 2 - ext.x_bearing,
                                   y + (ht - ext.height) / 2 - ext.y_bearing);
                cairo_show_text(cr, key_name(b->key_idx));
            }
        } else {
            gboolean sel = (a->selected_key == b->key_idx);
            snprintf(buf, sizeof buf, "%s", key_name(b->key_idx));
            draw_key(cr, x, y, wd, ht, buf, sel, FALSE);
        }
    }

    /* strip of keys that are not on the skin image (M1-M5, media row, ...) */
    {
        double x = KV_MARGIN * sx;
        double y = KV_STRIP_Y * sy;
        for (int i = 0; i < omni_hidden_key_count; i++) {
            int key = omni_hidden_keys[i];
            gboolean sel = (!kv->paint_mode && a->selected_key == key);
            draw_key(cr, x, y, KV_STRIP_SW * sx, KV_STRIP_SH * sy,
                     key_name(key), sel, TRUE);
            x += (KV_STRIP_SW + KV_STRIP_GAP) * sx;
        }
        /* legend */
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        cairo_set_font_size(cr, 8.0);
        cairo_move_to(cr, x + 8 * sx, y + 14 * sy);
        cairo_show_text(cr, kv->paint_mode
            ? (a->paint_raw
               ? "click keys to paint them with the selected colour (raw RGB)"
               : "click keys to paint them with the selected palette colour")
            : "extra keys not shown on the device image (M1-M5, media row, …)");
    }
    return FALSE;
}

static gboolean kb_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    KbView *kv = data;
    App *a = kv->app;
    GtkAllocation alloc;
    double lx, ly;

    gtk_widget_get_allocation(w, &alloc);
    if (alloc.width < 1 || alloc.height < 1) return FALSE;
    lx = ev->x / (alloc.width / KV_LOGICAL_W);
    ly = ev->y / (alloc.height / KV_LOGICAL_H);

    int hit = -1;
    for (int i = 0; i < omni_key_box_count; i++) {
        const struct omni_key_box *b = &omni_key_boxes[i];
        if (lx >= KV_MARGIN + b->x && lx <= KV_MARGIN + b->x + b->w &&
            ly >= KV_MARGIN + b->y && ly <= KV_MARGIN + b->y + b->h) {
            hit = b->key_idx;
            break;
        }
    }
    if (hit < 0) {
        double x = KV_MARGIN;
        for (int i = 0; i < omni_hidden_key_count; i++) {
            if (lx >= x && lx <= x + KV_STRIP_SW && ly >= KV_STRIP_Y &&
                ly <= KV_STRIP_Y + KV_STRIP_SH) {
                hit = omni_hidden_keys[i];
                break;
            }
            x += KV_STRIP_SW + KV_STRIP_GAP;
        }
    }
    if (hit < 0) return FALSE;

    if (kv->paint_mode && a->paint_raw) {
        int slot = app_key_to_slot(a, hit);
        if (slot < 0) {
            app_status(a, "Key %d (%s) has no device slot mapping.", hit, key_name(hit));
        } else if (!a->have_group) {
            app_status(a, "Read the group colours first (button above the keyboard).");
        } else if ((uint32_t)slot * 3 + 3 > OMNI_GROUP_MAXLEN) {
            app_status(a, "Slot %d is outside the colour page.", slot);
        } else {
            uint8_t *c = a->group_rgb + slot * 3;
            c[0] = a->paint_rgb[0];
            c[1] = a->paint_rgb[1];
            c[2] = a->paint_rgb[2];
            gtk_widget_queue_draw(w);
            app_status(a, "Painted %s (slot %d) #%02X%02X%02X — press Apply to write it.",
                       key_name(hit), slot, c[0], c[1], c[2]);
        }
    } else if (kv->paint_mode) {
        int slot = app_key_to_slot(a, hit);
        if (slot < 0) {
            app_status(a, "Key %d (%s) has no device slot mapping.", hit, key_name(hit));
        } else {
            a->ledidx[slot] = (uint8_t)a->paint_idx;
            gtk_widget_queue_draw(w);
            app_status(a, "Painted %s (slot %d) with palette index %d%s.",
                       key_name(hit), slot, a->paint_idx,
                       a->paint_idx == 0xFF ? " (off)" : "");
        }
    } else {
        a->selected_key = hit;
        gtk_widget_queue_draw(w);
        keys_page_selection_changed(a);
        app_status(a, "Selected key %d (%s).", hit, key_name(hit));
    }
    return TRUE;
}

GtkWidget *kb_view_new(App *a, int paint_mode)
{
    GtkWidget *da = gtk_drawing_area_new();
    KbView *kv = g_new0(KbView, 1);
    kv->app = a;
    kv->paint_mode = paint_mode;
    gtk_widget_set_size_request(da, 700, 280);
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
    g_object_set_data_full(G_OBJECT(da), "kbview", kv, g_free);
    g_signal_connect(da, "draw", G_CALLBACK(kb_draw), kv);
    g_signal_connect(da, "button-press-event", G_CALLBACK(kb_press), kv);
    gtk_widget_set_has_window(da, TRUE); /* ensure a native GdkWindow for events */
    return da;
}

void kb_view_refresh(GtkWidget *w)
{
    if (w) gtk_widget_queue_draw(w);
}

/* ------------------------------------------------------------------ */
/* Keys page                                                           */

void keys_page_selection_changed(App *a)
{
    char buf[256];
    int key = a->selected_key;

    if (key < 0) {
        gtk_label_set_markup(GTK_LABEL(a->sel_name_lbl), "<i>click a key above</i>");
        gtk_label_set_text(GTK_LABEL(a->sel_assign_lbl), "");
        return;
    }
    int slot = app_key_to_slot(a, key);
    snprintf(buf, sizeof buf, "<b>Key %d — %s</b>   (device slot %s)",
             key, key_name(key), slot >= 0 ? "?" : "?");
    /* slot number */
    if (slot >= 0)
        snprintf(buf, sizeof buf, "<b>Key %d — %s</b>   (device slot %d)",
                 key, key_name(key), slot);
    gtk_label_set_markup(GTK_LABEL(a->sel_name_lbl), buf);

    omni_assignment_text(a->assignment[key], buf, sizeof buf);
    gtk_label_set_text(GTK_LABEL(a->sel_assign_lbl), buf);

    /* populate the editor with the current assignment */
    const uint8_t *as = a->assignment[key];
    a->in_refresh = 1;
    if (as[0] == 0x20 && as[1] == 0 && as[2] == 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 4); /* disabled */
    } else if (as[0] == 0x20) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 0);
        int u = -1;
        for (int i = 0; i < omni_usage_count; i++)
            if (omni_usages[i].usage == as[2]) { u = i; break; }
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->usage_combo), u >= 0 ? u : 0);
        for (int m = 0; m < 8; m++)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mod_checks[m]),
                                         (as[1] >> m) & 1);
    } else if (as[0] == 0xA0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 1);
        char hex[8];
        snprintf(hex, sizeof hex, "%02X", as[1]);
        gtk_entry_set_text(GTK_ENTRY(a->media_entry), hex);
    } else if (as[0] == 0xD0 && as[1] == 0xA2) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 2);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->macro_combo),
                                 (as[2] >= 1 && as[2] <= 5) ? as[2] - 1 : 0);
    } else if (as[0] == 0x71 || as[0] == 0x70) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 3);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->mslot_spin), as[1]);
    }
    a->in_refresh = 0;
}

static void on_type_changed(GtkComboBox *cb, App *a)
{
    static const char *stack_names[] = { "normal", "media", "macro", "mslot", "off" };
    int i = gtk_combo_box_get_active(cb);
    if (i >= 0 && i < 5)
        gtk_stack_set_visible_child_name(GTK_STACK(a->type_stack), stack_names[i]);
}

static void on_set_assignment(App *a, GtkWidget *btn)
{
    (void)btn;
    int key = a->selected_key;
    if (key < 0) { app_status(a, "Select a key first."); return; }

    uint8_t as[3] = { 0x20, 0, 0 };
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(a->type_combo))) {
    case 0: { /* normal */
        int u = gtk_combo_box_get_active(GTK_COMBO_BOX(a->usage_combo));
        if (u < 0 || u >= omni_usage_count) { app_status(a, "Pick a usage."); return; }
        as[0] = 0x20;
        as[2] = omni_usages[u].usage;
        for (int m = 0; m < 8; m++)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->mod_checks[m])))
                as[1] |= (uint8_t)(1u << m);
        break;
    }
    case 1: { /* media */
        unsigned code = 0;
        const char *hex = gtk_entry_get_text(GTK_ENTRY(a->media_entry));
        if (sscanf(hex, "%x", &code) != 1 || code > 0xFF) {
            app_status(a, "Bad media code hex."); return;
        }
        as[0] = 0xA0; as[1] = (uint8_t)code;
        break;
    }
    case 2: { /* macro M1-M5 */
        int m = gtk_combo_box_get_active(GTK_COMBO_BOX(a->macro_combo));
        as[0] = 0xD0; as[1] = 0xA2; as[2] = (uint8_t)(m + 1);
        break;
    }
    case 3: { /* macro slot */
        int s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->mslot_spin));
        as[0] = 0x71; as[1] = (uint8_t)s;
        break;
    }
    default: /* disabled */
        as[0] = 0x20; as[1] = 0; as[2] = 0;
        break;
    }
    memcpy(a->assignment[key], as, 3);
    keys_page_selection_changed(a);
    app_status(a, "Key %d assignment set locally — use 'Apply' below to send "
                  "it to the keyboard.", key);
}

static void on_clear_assignment(App *a, GtkWidget *btn)
{
    (void)btn;
    int key = a->selected_key;
    if (key < 0) return;
    uint8_t as[3] = { 0x20, 0, 0 };
    memcpy(a->assignment[key], as, 3);
    keys_page_selection_changed(a);
    app_status(a, "Key %d disabled locally.", key);
}

static void on_read_live(App *a, GtkWidget *btn)
{
    (void)btn;
    uint16_t kc;
    int rc;
    if (!app_dev_check(a)) return;
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;

    rc = omni_begin(a->dev);
    if (rc == OMNI_OK) rc = omni_read_keyids(a->dev, a->keyids, (uint16_t)(kc * 3));
    if (rc == OMNI_OK) rc = omni_commit(a->dev);
    if (rc != OMNI_OK) { app_status(a, "cmd 07 failed: %s", omni_last_error()); return; }

    if (omni_build_keymap(a->keyids, kc, &a->km) == OMNI_OK)
        a->have_keymap = 1;
    for (int s = 0; s < kc; s++) {
        int k = app_slot_to_key(a, s);
        if (k >= 0) memcpy(a->assignment[k], a->keyids + s * 3, 3);
    }
    app_log(a, "live table (cmd 07) read: %u slots, keymap rebuilt", kc);
    kb_view_refresh(a->kb_select);
    keys_page_selection_changed(a);
    app_status(a, "Live table read (cmd 07) — %u slots.", kc);
}

static void on_read_layer(App *a, GtkWidget *btn)
{
    (void)btn;
    uint8_t page[OMNI_REMAP_PAGE];
    uint16_t kc;
    int layer, rc;
    if (!app_dev_check(a)) return;
    layer = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->layer_spin));
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;

    rc = omni_read_remap(a->dev, (uint8_t)layer, page, (uint16_t)(kc * 3));
    if (rc != OMNI_OK) { app_status(a, "cmd 0A failed: %s", omni_last_error()); return; }

    for (int s = 0; s < kc; s++) {
        int k = app_slot_to_key(a, s);
        if (k >= 0) memcpy(a->assignment[k], page + s * 3, 3);
    }
    char hex[3 * 8 + 1];
    hex_format(page, 8, hex, sizeof hex);
    app_log(a, "remap layer %d read (cmd 0A), first bytes: %s…", layer, hex);
    kb_view_refresh(a->kb_select);
    keys_page_selection_changed(a);
    app_status(a, "Layer %d read into the editor (cmd 0A).", layer);
}

static void on_write_layer(App *a, GtkWidget *btn)
{
    (void)btn;
    uint8_t page[OMNI_REMAP_PAGE];
    uint8_t back[OMNI_REMAP_PAGE];
    uint16_t kc;
    int layer, rc;
    if (!app_dev_check(a)) return;

    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;

    if (omni_supports_group_rgb(a->dev)) {
        /* 320F:505B: cmd 0A/0B pages 0-2 are the custom-colour groups, not key
         * assignments (PROTOCOL.md 5.2b/13.2), and the running key map is not
         * served from them at all. Write the keyboard's real persistent key
         * table (settings NVRAM bank 0, cmd 06) instead. */
        memset(page, 0, sizeof page);
        memcpy(page, a->keyids, (size_t)kc * 3);
        if (omni_overlay_remap_page(a->assignment, kc, &a->km, page, a->quirks) != OMNI_OK) {
            app_status(a, "cannot build assignment table"); return;
        }
        rc = omni_write_assign_bank(a->dev, 0, page, (uint16_t)(kc * 3), back);
        if (rc != OMNI_OK) {
            app_status(a, "key table write failed: %s", omni_last_error());
            app_log(a, "persistent key table write failed: %s", omni_last_error());
            return;
        }
        memcpy(a->keyids, back, (size_t)kc * 3);
        app_log(a, "persistent key table (NVRAM 0x0C0) written + verified, %u slots", kc);
        app_status(a, "Key table saved and verified. Unplug and re-plug the keyboard "
                      "to activate — this keyboard loads its key map at power-on.");
        kb_view_refresh(a->kb_select);
        return;
    }

    layer = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->layer_spin));
    if (omni_build_remap_page(a->assignment, kc, &a->km, page, a->quirks) != OMNI_OK) {
        app_status(a, "cannot build remap page"); return;
    }
    rc = omni_begin(a->dev);
    if (rc == OMNI_OK) rc = omni_write_remap(a->dev, (uint8_t)layer, page, (uint16_t)(kc * 3));
    if (rc == OMNI_OK) rc = omni_commit(a->dev);
    if (rc != OMNI_OK) { app_status(a, "cmd 0B failed: %s", omni_last_error()); return; }
    app_log(a, "remap layer %d written (cmd 0B), %u slots", layer, kc);
    app_status(a, "Layer %d written to the keyboard (cmd 0B).", layer);
}

static void on_apply_live(App *a, GtkWidget *btn)
{
    (void)btn;
    uint8_t page[OMNI_REMAP_PAGE];
    uint16_t kc;
    int rc;
    if (!app_dev_check(a)) return;
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;

    /* start from the table the device reported, so slots we have no key for
     * (knob, M6-M8, unused slots) are written back unchanged, not as 00 00 00 */
    memset(page, 0, sizeof page);
    memcpy(page, a->keyids, (size_t)kc * 3);
    if (omni_overlay_remap_page(a->assignment, kc, &a->km, page, a->quirks) != OMNI_OK) {
        app_status(a, "cannot build assignment table"); return;
    }

    if (omni_supports_group_rgb(a->dev)) {
        /* cmd 09 (the "second bank") is accepted on 320F:505B but the running
         * map does not follow it; the persistent table the firmware loads at
         * power-on lives in NVRAM bank 0 (PROTOCOL.md 13.3). */
        uint8_t back[OMNI_REMAP_PAGE];
        rc = omni_write_assign_bank(a->dev, 0, page, (uint16_t)(kc * 3), back);
        if (rc != OMNI_OK) {
            app_status(a, "key table write failed: %s", omni_last_error());
            app_log(a, "persistent key table write failed: %s", omni_last_error());
            return;
        }
        memcpy(a->keyids, back, (size_t)kc * 3);
        app_log(a, "persistent key table (NVRAM 0x0C0) written + verified, %u slots", kc);
        app_status(a, "Assignments saved and verified. Unplug and re-plug the keyboard "
                      "to activate — this keyboard loads its key map at power-on.");
        kb_view_refresh(a->kb_select);
        return;
    }

    rc = omni_begin(a->dev);
    if (rc == OMNI_OK) rc = omni_write_assignments(a->dev, page, (uint16_t)(kc * 3), 0);
    if (rc == OMNI_OK) rc = omni_commit(a->dev);
    if (rc != OMNI_OK) { app_status(a, "cmd 09 failed: %s", omni_last_error()); return; }
    memcpy(a->keyids, page, (size_t)kc * 3);
    app_log(a, "live assignment bank written (cmd 09), %u slots", kc);
    app_status(a, "Assignments applied to the live bank (cmd 09).");
}

GtkWidget *build_keys_page(App *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame, *hbox, *btn, *lbl;

    /* keyboard view */
    frame = gtk_frame_new("Keyboard — click a key to edit its assignment "
                          "(geometry from the original Windows skin)");
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    a->kb_select = kb_view_new(a, 0);
    gtk_container_add(GTK_CONTAINER(frame), a->kb_select);

    /* selected key + editor */
    frame = gtk_frame_new("Assignment editor (3-byte [T,X,P] records, PROTOCOL.md §6)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    a->sel_name_lbl = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(a->sel_name_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(a->sel_name_lbl), 0);
    gtk_box_pack_start(GTK_BOX(vbox), a->sel_name_lbl, FALSE, FALSE, 0);
    a->sel_assign_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(a->sel_assign_lbl), 0);
    gtk_box_pack_start(GTK_BOX(vbox), a->sel_assign_lbl, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    lbl = gtk_label_new("New assignment:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    a->type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->type_combo), "Normal key");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->type_combo), "Media / OEM code");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->type_combo), "Macro key M1–M5");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->type_combo), "Macro slot (type 0x71)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->type_combo), "Disabled");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->type_combo), 0);
    g_signal_connect(a->type_combo, "changed", G_CALLBACK(on_type_changed), a);
    gtk_box_pack_start(GTK_BOX(hbox), a->type_combo, FALSE, FALSE, 0);

    a->type_stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(a->type_stack), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), a->type_stack, TRUE, TRUE, 0);

    /* normal key sub-form */
    {
        GtkWidget *h2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        a->usage_combo = gtk_combo_box_text_new();
        for (int i = 0; i < omni_usage_count; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->usage_combo),
                                           omni_usages[i].name);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->usage_combo), 0);
        gtk_box_pack_start(GTK_BOX(h2), a->usage_combo, FALSE, FALSE, 0);
        for (int m = 0; m < 8; m++) {
            a->mod_checks[m] = gtk_check_button_new_with_label(omni_mod_names[m]);
            gtk_box_pack_start(GTK_BOX(h2), a->mod_checks[m], FALSE, FALSE, 0);
        }
        gtk_widget_show_all(h2);
        gtk_stack_add_named(GTK_STACK(a->type_stack), h2, "normal");
    }
    /* media sub-form */
    {
        a->media_entry = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(a->media_entry), 8);
        gtk_entry_set_placeholder_text(GTK_ENTRY(a->media_entry), "hex, e.g. 1B");
        gtk_widget_show(a->media_entry);
        gtk_stack_add_named(GTK_STACK(a->type_stack), a->media_entry, "media");
    }
    /* macro M1-M5 */
    {
        a->macro_combo = gtk_combo_box_text_new();
        for (int m = 1; m <= 5; m++) {
            char name[8];
            snprintf(name, sizeof name, "M%d", m);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->macro_combo), name);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->macro_combo), 0);
        gtk_widget_show(a->macro_combo);
        gtk_stack_add_named(GTK_STACK(a->type_stack), a->macro_combo, "macro");
    }
    /* macro slot */
    {
        a->mslot_spin = gtk_spin_button_new_with_range(0, 255, 1);
        gtk_widget_show(a->mslot_spin);
        gtk_stack_add_named(GTK_STACK(a->type_stack), a->mslot_spin, "mslot");
    }
    /* disabled */
    {
        lbl = gtk_label_new("key sends nothing");
        gtk_widget_show(lbl);
        gtk_stack_add_named(GTK_STACK(a->type_stack), lbl, "off");
    }
    gtk_stack_set_visible_child_name(GTK_STACK(a->type_stack), "normal");

    btn = gtk_button_new_with_label("Set on selected key");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_set_assignment), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Clear");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_clear_assignment), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* apply frame */
    frame = gtk_frame_new("Apply / read from device");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), hbox);

    lbl = gtk_label_new("Layer:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    a->layer_spin = gtk_spin_button_new_with_range(0, 7, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->layer_spin), 1);
    gtk_box_pack_start(GTK_BOX(hbox), a->layer_spin, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Read live table (cmd 07)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_read_live), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Read layer (cmd 0A)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_read_layer), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Write layer (cmd 0B)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_write_layer), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Apply to device (persistent)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_apply_live), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);

    lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "<i>320F:505B: 'Apply to device' writes the keyboard's persistent key table "
        "(settings NVRAM bank 0, cmd 06) and verifies it by read-back. This keyboard "
        "loads its key map at power-on, so unplug/re-plug to activate. "
        "'Write layer (cmd 0B)' is the stored-layer path for other models. "
        "Key↔slot translation uses the cmd-07 table; model 0x14 applies the "
        "Enter/Backslash slot-swap on writes.</i>");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    a->selected_key = -1;
    gtk_widget_show_all(box);
    return box;
}
