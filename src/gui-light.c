/*
 * gui-light.c — lighting page: global effects + custom per-key palette
 *
 * Global effects edit the 34-byte settings record (PROTOCOL.md §4);
 * custom per-key lighting uploads the 13-colour palette (report 0xFF),
 * the per-LED palette-index table (cmd 12) and switches to mode 20.
 */
#include "gui.h"

#include <stdio.h>
#include <string.h>

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

    /* ---- custom per-key ---- */
    frame = gtk_frame_new("Custom per-key lighting (mode 20 — palette + per-LED index)");
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

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
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 4);

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

    lbl = gtk_label_new("Custom group:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 8);
    a->group_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 1");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 2");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->group_combo), "Group 3");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->group_combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), a->group_combo, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Read per-key colours (cmd 1B)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_custom_read), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 8);
    btn = gtk_button_new_with_label("Apply custom lighting");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_custom_apply), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    a->kb_paint = kb_view_new(a, 1);
    gtk_box_pack_start(GTK_BOX(vbox), a->kb_paint, TRUE, TRUE, 0);

    gtk_widget_show_all(box);
    return box;
}
