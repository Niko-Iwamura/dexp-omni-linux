/*
 * gui.h — shared state for the GTK configurator (omni-gtk)
 */
#ifndef OMNI_GUI_H
#define OMNI_GUI_H

#include "omni.h"
#include <gtk/gtk.h>

#define GUI_MACRO_MAX  16
#define GUI_EVENTS_MAX 256

typedef struct App {
    /* device state */
    omni_dev            *dev;
    int                  have_dev;
    uint8_t              info[34];
    unsigned             quirks;          /* OMNI_QUIRK_* */
    struct omni_keymap   km;
    int                  have_keymap;
    uint8_t              keyids[512];     /* cmd-07 live table, slot-major  */
    uint8_t              assignment[OMNI_KEYREF_COUNT][3];
    uint8_t              ledidx[OMNI_MAX_KEYS];
    int                  have_ledidx;
    uint8_t              palette[OMNI_PALETTE_COUNT][3];
    /* 320F:505B raw-RGB path: one 384-byte page (128 slots x 3) per group */
    uint8_t              group_rgb[OMNI_GROUP_MAXLEN];
    int                  have_group;
    int                  group_shown;
    uint8_t              paint_rgb[3];    /* colour painted with, raw-RGB mode */
    int                  paint_raw;       /* 1 = kb_paint shows group_rgb */
    struct omni_settings settings;
    int                  have_settings;
    guint32              macro_events[GUI_MACRO_MAX][GUI_EVENTS_MAX];
    int                  macro_counts[GUI_MACRO_MAX];
    int                  macro_total;
    int                  in_refresh;      /* suppress signal handlers */

    /* widgets */
    GtkWidget *win, *nb, *status;
    GtkTextBuffer *logbuf;
    GtkWidget *log_view;

    /* device page */
    GtkWidget *node_combo, *dev_info_lbl, *conn_lbl;
    GtkWidget *poll_combo, *key6_sw, *win_sw, *wflag_sw;

    /* lighting page */
    GtkWidget *mode_combo, *bright_scale, *speed_scale, *color_btn, *multi_sw;
    GtkWidget *fx_spin, *group_combo, *paint_combo;
    GtkWidget *palette_btns[OMNI_PALETTE_COUNT];
    GtkWidget *kb_paint;
    GtkWidget *rgb_btn, *rgb_read_btn, *rgb_apply_btn;
    GtkWidget *cust_vbox, *rgb_child, *pal_child, *kb_rgb;

    /* keys page */
    GtkWidget *kb_select;
    GtkWidget *sel_name_lbl, *sel_assign_lbl;
    GtkWidget *type_combo, *type_stack;
    GtkWidget *usage_combo, *mod_checks[8];
    GtkWidget *media_entry, *macro_combo, *mslot_spin;
    GtkWidget *layer_spin;

    /* macro page */
    GtkWidget *macro_count_spin, *macro_idx_spin, *macro_view;
    GtkListStore *macro_store;
    GtkWidget *macro_cap_lbl;

    int selected_key;      /* key index for the assignment editor */
    int paint_idx;         /* palette index painted in custom mode, 0xFF=off */
} App;

/* gui-main.c */
void app_log(App *a, const char *fmt, ...);
void app_status(App *a, const char *fmt, ...);
int  app_dev_check(App *a);
int  app_connect(App *a, const char *node);
void app_disconnect(App *a);
void app_refresh_all(App *a);
int  hex_parse(const char *s, uint8_t *out, int maxlen);
void hex_format(const uint8_t *b, unsigned n, char *out, size_t cap);
int  app_key_to_slot(App *a, int key);
int  app_slot_to_key(App *a, int slot);

/* page builders */
GtkWidget *build_device_page(App *a);
GtkWidget *build_light_page(App *a);
/* switch the custom per-key section between the raw-RGB and palette paths */
void app_set_custom_path(App *a, int raw_rgb);
GtkWidget *build_keys_page(App *a);
GtkWidget *build_macro_page(App *a);
GtkWidget *build_console_page(App *a);

/* kb view (gui-keys.c) */
GtkWidget *kb_view_new(App *a, int paint_mode);
void       kb_view_refresh(GtkWidget *w);
void       keys_page_selection_changed(App *a);

/* macro page (gui-macro.c) */
void macro_refresh(App *a);

#endif
