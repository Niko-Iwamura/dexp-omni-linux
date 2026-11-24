/*
 * gui-light.c — lighting page: global effects + custom per-key colours
 *
 * Global effects edit the 34-byte settings record (PROTOCOL.md §4).
 * Custom per-key colours have two device-dependent paths (§5.2b):
 *   - 0C45:1007 — 13-colour palette (report 0xFF), per-LED index table
 *     (cmd 12/1B), mode 20;
 *   - 320F:505B — no palette: a raw RGB triple per slot in a 384-byte page
 *     per custom group at 0x6C0/0x8C0/0xAC0 (cmd 05/06), mode 20.
 */
#include "gui.h"

#include <stdio.h>
#include <string.h>

/* Show the child that matches the connected keyboard's per-key colour path.
 * Called at page build time (no device yet -> palette child) and again from
 * app_connect()/app_disconnect() (PROTOCOL.md 5.2b). */
void app_set_custom_path(App *a, int raw_rgb)
{
    if (!a->cust_vbox || !a->rgb_child || !a->pal_child) return;
    a->paint_raw = raw_rgb ? 1 : 0;
    if (raw_rgb) {
        if (gtk_widget_get_parent(a->rgb_child) != a->cust_vbox)
            gtk_box_pack_start(GTK_BOX(a->cust_vbox), a->rgb_child, TRUE, TRUE, 0);
        gtk_widget_show_all(a->rgb_child);
        gtk_widget_hide(a->pal_child);
    } else {
        if (gtk_widget_get_parent(a->pal_child) != a->cust_vbox)
            gtk_box_pack_start(GTK_BOX(a->cust_vbox), a->pal_child, TRUE, TRUE, 0);
        gtk_widget_show_all(a->pal_child);
        gtk_widget_hide(a->rgb_child);
    }
}

/* ---- 320F:505B raw-RGB page helpers ---------------------------------- */

static int light_group_index(App *a)
{
    int g = a->group_combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(a->group_combo)) : 0;
    return g < 0 ? 0 : g;
}

static uint16_t light_page_len(App *a)
{
    uint16_t kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;
    uint16_t len = (uint16_t)(kc * 3);
    return len > OMNI_GROUP_MAXLEN ? OMNI_GROUP_MAXLEN : len;
}

static void on_rgb_read(App *a, GtkWidget *btn)
{
    int group = light_group_index(a);
    uint16_t len = light_page_len(a);
    int rc;
    (void)btn;
    if (!app_dev_check(a)) return;

    rc = omni_read_group(a->dev, (uint8_t)group, a->group_rgb, len);
    if (rc != OMNI_OK) {
        app_status(a, "read group %d failed: %s", group, omni_last_error());
        return;
    }
    a->have_group = 1;
    a->group_shown = group;
    kb_view_refresh(a->kb_paint);
    kb_view_refresh(a->kb_rgb);
    app_log(a, "group %d colour page read (cmd 05 @0x%03X), %u slots",
            group, OMNI_GROUP_OFF0 + group * OMNI_REMAP_PAGE, len / 3);
    app_status(a, "Per-key colours of group %d read from the keyboard.", group);
}

static void on_rgb_pick(GtkColorButton *btn, gpointer user)
{
    App *a = user;
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &rgba);
    a->paint_rgb[0] = (uint8_t)(rgba.red * 255 + 0.5);
    a->paint_rgb[1] = (uint8_t)(rgba.green * 255 + 0.5);
    a->paint_rgb[2] = (uint8_t)(rgba.blue * 255 + 0.5);
    kb_view_refresh(a->kb_rgb);
}

static void on_rgb_apply(App *a, GtkWidget *btn)
{
    int group = light_group_index(a);
    uint16_t len = light_page_len(a);
    struct omni_settings s;
    int rc;
    (void)btn;
    if (!app_dev_check(a)) return;
    if (!a->have_group) {
        app_status(a, "Read group %d first, otherwise the page you write would be stale.", group);
        return;
    }
    if (a->group_shown != group) {
        app_status(a, "The page on screen is group %d but group %d is selected — read it first.",
                   a->group_shown, group);
        return;
    }

    rc = omni_write_group_verified(a->dev, (uint8_t)group, a->group_rgb, len, 1);
    if (rc != OMNI_OK) {
        app_status(a, "apply failed: %s", omni_last_error());
        return;
    }

    if (omni_load_settings(a->dev, &s) != OMNI_OK) {
        app_status(a, "group page written, but loading the settings record failed: %s",
                   omni_last_error());
        return;
    }
    if (s.mode != OMNI_CUSTOM_MODE || s.custom_group != (uint8_t)group) {
        s.mode = OMNI_CUSTOM_MODE;
        s.custom_group = (uint8_t)group;
        if (omni_save_settings_verified(a->dev, &s) != OMNI_OK) {
            app_status(a, "group page written, but switching to Customize failed: %s",
                       omni_last_error());
            return;
        }
    }
    a->settings = s;
    a->have_settings = 1;
    app_log(a, "group %d applied: %u slots, verified by read-back; mode 20, group %d",
            group, len / 3, group);
    app_status(a, "Group %d written and verified; keyboard switched to Customize.", group);
    app_refresh_all(a);
}

static void on_group_changed(GtkComboBox *cb, App *a)
{
    int g = gtk_combo_box_get_active(cb);
    if (g < 0) return;
    if (a->have_group && a->group_shown != g) {
        app_status(a, "Group %d selected — press Read group colours before painting it "
                       "(showing group %d).", g, a->group_shown);
    }
}

static void on_light_read(App *a, GtkWidget *btn)
{
    (void)btn;
    if (!app_dev_check(a)) return;
    if (omni_load_settings(a->dev, &a->settings) != OMNI_OK) {
        app_status(a, "load settings failed: %s", omni_last_error());
        return;
    }
    a->have_settings = 1;
    app_refresh_all(a);
    app_status(a, "Lighting read from device (settings slot %u, mode %u — %s).",
               a->settings.slot, a->settings.mode,
               omni_mode_names[a->settings.mode < 32 ? a->settings.mode : 0] ?: "?");
}

static void on_light_apply(App *a, GtkWidget *btn)
{
    struct omni_settings s;
    (void)btn;
    if (!app_dev_check(a)) return;

    if (a->have_settings)
        s = a->settings;
    else if (omni_load_settings(a->dev, &s) != OMNI_OK) {
        app_status(a, "load settings failed: %s", omni_last_error());
        return;
    }

    {
        uint8_t ids[32];
        int n = omni_selectable_modes(ids);
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(a->mode_combo));
        if (idx >= 0 && idx < n) s.mode = ids[idx];
    }
    s.brightness = (uint8_t)gtk_range_get_value(GTK_RANGE(a->bright_scale));
    s.speed = (uint8_t)gtk_range_get_value(GTK_RANGE(a->speed_scale));
    s.fx = (uint8_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->fx_spin));
    s.multicolor = gtk_switch_get_active(GTK_SWITCH(a->multi_sw)) ? 1 : 0;

    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(a->color_btn), &rgba);
    s.red = (uint8_t)(rgba.red * 255 + 0.5);
    s.green = (uint8_t)(rgba.green * 255 + 0.5);
    s.blue = (uint8_t)(rgba.blue * 255 + 0.5);

    if (omni_save_settings_verified(a->dev, &s) != OMNI_OK) {
        app_status(a, "apply failed: %s", omni_last_error());
        return;
    }
    a->settings = s;
    a->have_settings = 1;
    app_log(a, "lighting applied: mode %u (%s), brightness %u, speed %u, RGB %02X%02X%02X",
            s.mode, omni_mode_names[s.mode < 32 ? s.mode : 0] ?: "?",
            s.brightness, s.speed, s.red, s.green, s.blue);
    app_status(a, "Lighting applied.");
}

typedef struct PaletteCbData {
    App *app;
    int  idx;
} PaletteCbData;

static void palette_data_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void on_palette_set(GtkColorButton *btn, gpointer user)
{
    PaletteCbData *d = user;
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &rgba);
    d->app->palette[d->idx][0] = (uint8_t)(rgba.red * 255 + 0.5);
    d->app->palette[d->idx][1] = (uint8_t)(rgba.green * 255 + 0.5);
    d->app->palette[d->idx][2] = (uint8_t)(rgba.blue * 255 + 0.5);
    kb_view_refresh(d->app->kb_paint);
}

static void on_paint_combo(GtkComboBox *cb, App *a)
{
    int i = gtk_combo_box_get_active(cb);
    if (i <= 0) a->paint_idx = 0xFF;        /* Off */
    else a->paint_idx = i - 1;              /* palette 0..12 */
}

static void on_custom_read(App *a, GtkWidget *btn)
{
    uint16_t kc;
    int rc;
    (void)btn;
    if (!app_dev_check(a)) return;
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;
    rc = omni_begin(a->dev);
    if (rc == OMNI_OK) rc = omni_read_ledidx(a->dev, a->ledidx, kc);
    if (rc == OMNI_OK) rc = omni_commit(a->dev);
    if (rc != OMNI_OK) { app_status(a, "cmd 1B failed: %s", omni_last_error()); return; }
    a->have_ledidx = 1;
    kb_view_refresh(a->kb_paint);
    app_log(a, "per-LED palette table read (cmd 1B), %u LEDs", kc);
    app_status(a, "Per-key colours read from the keyboard (cmd 1B).");
}

static void on_custom_apply(App *a, GtkWidget *btn)
{
    uint16_t kc;
    int group, rc;
    (void)btn;
    if (!app_dev_check(a)) return;
    kc = a->info[5];
    if (kc > OMNI_MAX_KEYS) kc = OMNI_MAX_KEYS;
    group = gtk_combo_box_get_active(GTK_COMBO_BOX(a->group_combo));
    if (group < 0) group = 0;

    rc = omni_apply_custom(a->dev, a->palette, a->ledidx, kc, (uint8_t)group);
    if (rc != OMNI_OK) {
        app_status(a, "custom lighting failed: %s", omni_last_error());
        return;
    }
    if (omni_load_settings(a->dev, &a->settings) == OMNI_OK)
        a->have_settings = 1;
    app_log(a, "custom lighting applied: palette uploaded, %u LED indices (group %d), mode 20",
            kc, group);
    app_status(a, "Custom per-key lighting applied (palette + cmd 12 + mode 20).");
}

GtkWidget *build_light_page(App *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame, *hbox, *lbl, *btn;

    /* ---- global effects ---- */
    frame = gtk_frame_new("Global effect (settings record — PROTOCOL.md §4, §5.1)");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    lbl = gtk_label_new("Effect:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->mode_combo = gtk_combo_box_text_new();
    {
        uint8_t ids[32];
        int n = omni_selectable_modes(ids);
        for (int i = 0; i < n; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->mode_combo),
                                           omni_mode_names[ids[i]]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->mode_combo), 0);
    gtk_widget_set_hexpand(a->mode_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->mode_combo, 1, 0, 1, 1);

    lbl = gtk_label_new("Brightness (0–4):");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->bright_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 4, 1);
    gtk_scale_set_draw_value(GTK_SCALE(a->bright_scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(a->bright_scale), GTK_POS_BOTTOM);
    gtk_widget_set_hexpand(a->bright_scale, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->bright_scale, 3, 0, 1, 1);

    lbl = gtk_label_new("Speed (0–4):");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->speed_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 4, 1);
    gtk_scale_set_draw_value(GTK_SCALE(a->speed_scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(a->speed_scale), GTK_POS_BOTTOM);
    gtk_widget_set_hexpand(a->speed_scale, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->speed_scale, 1, 1, 1, 1);

    lbl = gtk_label_new("Colour:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->color_btn = gtk_color_button_new();
    {
        GdkRGBA c0 = { 1.0, 0.4, 0.0, 1.0 };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(a->color_btn), &c0);
    }
    gtk_grid_attach(GTK_GRID(grid), lbl, 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->color_btn, 3, 1, 1, 1);

    lbl = gtk_label_new("Multi-colour:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->multi_sw = gtk_switch_new();
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->multi_sw, 1, 2, 1, 1);

    lbl = gtk_label_new("Direction / FX byte:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    a->fx_spin = gtk_spin_button_new_with_range(0, 255, 1);
    gtk_widget_set_tooltip_text(a->fx_spin,
        "Settings record byte 4 (Fx / direction). Semantics per effect — "
        "raw value, see PROTOCOL.md §4.");
    gtk_grid_attach(GTK_GRID(grid), lbl, 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->fx_spin, 3, 2, 1, 1);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    btn = gtk_button_new_with_label("Read from keyboard");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_light_read), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Apply");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_light_apply), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), hbox, 1, 3, 3, 1);

    /* ---- custom per-key ----
     * Both paths exist and are switched at connect time by app_set_custom_path():
     * the page is built before the device is known, so we cannot branch here. */
    frame = gtk_frame_new("Custom per-key colours");
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    {
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
        gtk_container_add(GTK_CONTAINER(frame), vbox);
        a->cust_vbox = vbox;

        /* raw-RGB child (320F:505B) */
        a->rgb_child = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *rh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        lbl = gtk_label_new("Custom group:");
        gtk_box_pack_start(GTK_BOX(rh), lbl, FALSE, FALSE, 0);
        a->group_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 1");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 2");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 3");
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->group_combo), 0);
        g_signal_connect(a->group_combo, "changed", G_CALLBACK(on_group_changed), a);
        gtk_box_pack_start(GTK_BOX(rh), a->group_combo, FALSE, FALSE, 0);

        lbl = gtk_label_new("Paint with:");
        gtk_box_pack_start(GTK_BOX(rh), lbl, FALSE, FALSE, 12);
        a->rgb_btn = gtk_color_button_new();
        {
            GdkRGBA c0 = { 1.0, 1.0, 1.0, 1.0 };
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(a->rgb_btn), &c0);
        }
        gtk_color_button_set_title(GTK_COLOR_BUTTON(a->rgb_btn), "Key colour");
        g_signal_connect(a->rgb_btn, "color-set", G_CALLBACK(on_rgb_pick), a);
        gtk_box_pack_start(GTK_BOX(rh), a->rgb_btn, FALSE, FALSE, 0);

        a->rgb_read_btn = gtk_button_new_with_label("Read group colours");
        g_signal_connect_swapped(a->rgb_read_btn, "clicked", G_CALLBACK(on_rgb_read), a);
        gtk_box_pack_start(GTK_BOX(rh), a->rgb_read_btn, FALSE, FALSE, 12);
        a->rgb_apply_btn = gtk_button_new_with_label("Apply group (verified write)");
        g_signal_connect_swapped(a->rgb_apply_btn, "clicked", G_CALLBACK(on_rgb_apply), a);
        gtk_box_pack_start(GTK_BOX(rh), a->rgb_apply_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(a->rgb_child), rh, FALSE, FALSE, 0);

        a->kb_rgb = kb_view_new(a, 1);
        gtk_box_pack_start(GTK_BOX(a->rgb_child), a->kb_rgb, TRUE, TRUE, 0);

        /* palette child (0C45:1007) */
        a->pal_child = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    lbl = gtk_label_new("13-colour palette:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    for (int i = 0; i < OMNI_PALETTE_COUNT; i++) {
        GdkRGBA c = { a->palette[i][0] / 255.0, a->palette[i][1] / 255.0,
                      a->palette[i][2] / 255.0, 1.0 };
        a->palette_btns[i] = gtk_color_button_new_with_rgba(&c);
        gtk_color_button_set_title(GTK_COLOR_BUTTON(a->palette_btns[i]),
                                   "Palette colour");
        PaletteCbData *d = g_new(PaletteCbData, 1);
        d->app = a; d->idx = i;
        g_signal_connect_data(a->palette_btns[i], "color-set",
                              G_CALLBACK(on_palette_set), d,
                              palette_data_free, 0);
        gtk_box_pack_start(GTK_BOX(hbox), a->palette_btns[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(a->pal_child), hbox, FALSE, FALSE, 4);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    lbl = gtk_label_new("Paint keys with:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    a->paint_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->paint_combo), "Off");
    for (int i = 0; i < OMNI_PALETTE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof name, "Colour %d", i + 1);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->paint_combo), name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->paint_combo), 1);
    g_signal_connect(a->paint_combo, "changed", G_CALLBACK(on_paint_combo), a);
    gtk_box_pack_start(GTK_BOX(a->pal_child), hbox, FALSE, FALSE, 4);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    lbl = gtk_label_new("Paint keys with:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    a->paint_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->paint_combo), "Off");
    for (int i = 0; i < OMNI_PALETTE_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof name, "Colour %d", i + 1);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->paint_combo), name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->paint_combo), 1);
    g_signal_connect(a->paint_combo, "changed", G_CALLBACK(on_paint_combo), a);
    gtk_box_pack_start(GTK_BOX(hbox), a->paint_combo, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Read per-key colours (cmd 1B)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_custom_read), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 8);
    btn = gtk_button_new_with_label("Apply custom lighting");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_custom_apply), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(a->pal_child), hbox, FALSE, FALSE, 0);

    a->kb_paint = kb_view_new(a, 1);
    gtk_box_pack_start(GTK_BOX(a->pal_child), a->kb_paint, TRUE, TRUE, 0);

        gtk_widget_show_all(box);
        app_set_custom_path(
            a, a->dev ? omni_supports_group_rgb(a->dev) : 0);
        return box;
    }
}
