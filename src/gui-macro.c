/*
 * gui-macro.c — macro table editor
 *
 * The macro table is a self-describing 0x55AA image uploaded with cmd 15
 * (PROTOCOL.md §7): 16-byte header, u16 offset table, then per macro a
 * u16 event count + count * u32 events. The exact bitfield layout of the
 * 4-byte event records is the last unknown of this protocol — it is being
 * decoded from .mac/macro.config captures (see PROTOCOL.md "open points"),
 * so events are edited as raw 32-bit words here.
 */
#include "gui.h"

#include <stdio.h>
#include <string.h>

enum { COL_N, COL_EVENT, COL_BYTES, COLS };

static int cur_macro(App *a)
{
    int i = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->macro_idx_spin));
    if (i < 0) i = 0;
    if (i >= GUI_MACRO_MAX) i = GUI_MACRO_MAX - 1;
    return i;
}

void macro_refresh(App *a)
{
    GtkTreeIter iter;
    int m, i;

    if (!a->macro_store) return;
    gtk_list_store_clear(a->macro_store);

    if (a->macro_total < 1) a->macro_total = 1;
    m = cur_macro(a);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(a->macro_idx_spin),
                              0, a->macro_total - 1);

    for (i = 0; i < a->macro_counts[m]; i++) {
        guint32 ev = a->macro_events[m][i];
        char hex[16], bytes[32];
        snprintf(hex, sizeof hex, "0x%08X", ev);
        snprintf(bytes, sizeof bytes, "%02X %02X %02X %02X",
                 (ev) & 0xFF, (ev >> 8) & 0xFF, (ev >> 16) & 0xFF, (ev >> 24) & 0xFF);
        gtk_list_store_append(a->macro_store, &iter);
        gtk_list_store_set(a->macro_store, &iter,
                           COL_N, i + 1, COL_EVENT, hex, COL_BYTES, bytes, -1);
    }
}

static void on_macro_count(GtkSpinButton *sb, App *a)
{
    a->macro_total = gtk_spin_button_get_value_as_int(sb);
    if (a->macro_total < 1) a->macro_total = 1;
    macro_refresh(a);
}

static void on_macro_idx(GtkSpinButton *sb, App *a)
{
    (void)sb;
    macro_refresh(a);
}

static void on_add_event(App *a, GtkWidget *btn)
{
    (void)btn;
    int m = cur_macro(a);
    if (a->macro_counts[m] >= GUI_EVENTS_MAX) {
        app_status(a, "Event limit reached for this macro.");
        return;
    }
    a->macro_events[m][a->macro_counts[m]++] = 0;
    macro_refresh(a);
}

static void on_remove_event(App *a, GtkWidget *btn)
{
    (void)btn;
    int m = cur_macro(a);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->macro_view));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        gint idx;
        gtk_tree_model_get(model, &iter, COL_N, &idx, -1);
        idx -= 1;
        if (idx >= 0 && idx < a->macro_counts[m]) {
            memmove(&a->macro_events[m][idx], &a->macro_events[m][idx + 1],
                    (size_t)(a->macro_counts[m] - idx - 1) * sizeof(guint32));
            a->macro_counts[m]--;
            macro_refresh(a);
        }
    } else {
        app_status(a, "Select an event row first.");
    }
}

static void on_event_edited(GtkCellRendererText *cell, gchar *path, gchar *text, App *a)
{
    (void)cell;
    GtkTreeIter iter;
    int m = cur_macro(a);
    unsigned v;
    if (sscanf(text, "%x", &v) != 1) { app_status(a, "Bad hex value."); return; }
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(a->macro_store), &iter, path)) {
        gint idx;
        gtk_tree_model_get(GTK_TREE_MODEL(a->macro_store), &iter, COL_N, &idx, -1);
        if (idx >= 1 && idx <= a->macro_counts[m]) {
            a->macro_events[m][idx - 1] = v;
            macro_refresh(a);
        }
    }
}

/* build the 0x55AA image exactly as OMNI.exe's 0x4966C0 does */
static uint8_t *macro_build_image(App *a, int *out_len)
{
    int n = a->macro_total;
    unsigned total = 0x10 + 2u * n;
    uint8_t *img;
    unsigned cursor, i, j;

    for (i = 0; i < (unsigned)n; i++)
        total += 4u + 4u * (unsigned)a->macro_counts[i];

    img = g_malloc0(total);

    /* header */
    img[0] = 0xAA; img[1] = 0x55;                /* u16 LE 0x55AA */
    img[2] = (uint8_t)(total & 0xFF);
    img[3] = (uint8_t)(total >> 8);
    img[4] = (uint8_t)(n & 0xFF);
    img[5] = (uint8_t)(n >> 8);
    /* bytes 6..15 zeroed (the Windows app leaves heap garbage — do better) */

    cursor = 0x10 + 2u * n;
    for (i = 0; i < (unsigned)n; i++) {
        img[0x10 + i * 2] = (uint8_t)(cursor & 0xFF);
        img[0x10 + i * 2 + 1] = (uint8_t)(cursor >> 8);
        img[cursor] = (uint8_t)(a->macro_counts[i] & 0xFF);
        img[cursor + 1] = (uint8_t)((unsigned)a->macro_counts[i] >> 8);
        /* img[cursor+2..3] reserved, zeroed */
        for (j = 0; j < (unsigned)a->macro_counts[i]; j++) {
            guint32 ev = a->macro_events[i][j];
            img[cursor + 4 + j * 4 + 0] = (uint8_t)(ev);
            img[cursor + 4 + j * 4 + 1] = (uint8_t)(ev >> 8);
            img[cursor + 4 + j * 4 + 2] = (uint8_t)(ev >> 16);
            img[cursor + 4 + j * 4 + 3] = (uint8_t)(ev >> 24);
        }
        cursor += 4u + 4u * (unsigned)a->macro_counts[i];
    }

    *out_len = (int)total;
    return img;
}

static void on_upload(App *a, GtkWidget *btn)
{
    uint8_t *img;
    int len, rc;
    uint16_t cap;
    (void)btn;
    if (!app_dev_check(a)) return;

    cap = omni_macro_capacity(a->dev);
    img = macro_build_image(a, &len);
    if (cap && (unsigned)len > cap) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(a->win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Macro table too large: %d bytes > %u bytes of on-board storage.\n"
            "Remove some events and try again.", len, cap);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(img);
        return;
    }

    rc = omni_begin(a->dev);
    if (rc == OMNI_OK) rc = omni_write_macros(a->dev, img, (uint16_t)len);
    if (rc == OMNI_OK) rc = omni_commit(a->dev);
    g_free(img);
    if (rc != OMNI_OK) {
        app_status(a, "macro upload (cmd 15) failed: %s", omni_last_error());
        return;
    }
    app_log(a, "macro table uploaded (cmd 15): %d macros, %d bytes", a->macro_total, len);
    app_status(a, "Macro table uploaded (cmd 15). Bind keys via the Keys page "
                  "(M1–M5 or macro-slot type 0x71).");
}

GtkWidget *build_macro_page(App *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *frame, *hbox, *lbl, *btn;

    frame = gtk_frame_new("Macro definitions (cmd 15 — 0x55AA table image)");
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    a->macro_cap_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(a->macro_cap_lbl), 0);
    gtk_box_pack_start(GTK_BOX(vbox), a->macro_cap_lbl, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    lbl = gtk_label_new("Number of macros:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    a->macro_count_spin = gtk_spin_button_new_with_range(1, GUI_MACRO_MAX, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->macro_count_spin), 1);
    g_signal_connect(a->macro_count_spin, "value-changed", G_CALLBACK(on_macro_count), a);
    gtk_box_pack_start(GTK_BOX(hbox), a->macro_count_spin, FALSE, FALSE, 0);

    lbl = gtk_label_new("Editing macro:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 8);
    a->macro_idx_spin = gtk_spin_button_new_with_range(0, GUI_MACRO_MAX - 1, 1);
    g_signal_connect(a->macro_idx_spin, "value-changed", G_CALLBACK(on_macro_idx), a);
    gtk_box_pack_start(GTK_BOX(hbox), a->macro_idx_spin, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Add event");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_add_event), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 8);
    btn = gtk_button_new_with_label("Remove selected");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_remove_event), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("Upload to keyboard (cmd 15)");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(on_upload), a);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* event list */
    a->macro_store = gtk_list_store_new(COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    a->macro_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->macro_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->macro_view), TRUE);

    GtkCellRendererText *rnd = GTK_CELL_RENDERER_TEXT(gtk_cell_renderer_text_new());
    GtkTreeViewColumn *col;
    col = gtk_tree_view_column_new_with_attributes("#", GTK_CELL_RENDERER(rnd),
                                                   "text", COL_N, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->macro_view), col);

    GtkCellRendererText *rnd_edit = GTK_CELL_RENDERER_TEXT(gtk_cell_renderer_text_new());
    g_object_set(rnd_edit, "editable", TRUE, NULL);
    g_signal_connect(rnd_edit, "edited", G_CALLBACK(on_event_edited), a);
    col = gtk_tree_view_column_new_with_attributes("Event (u32 LE on wire)",
            GTK_CELL_RENDERER(rnd_edit), "text", COL_EVENT, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->macro_view), col);

    col = gtk_tree_view_column_new_with_attributes("bytes",
            GTK_CELL_RENDERER(gtk_cell_renderer_text_new()), "text", COL_BYTES, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->macro_view), col);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), a->macro_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "<i>Events are raw 32-bit words. Their bitfield layout (key / delay) is the "
        "last unknown of the protocol — see PROTOCOL.md §7 'open points'. "
        "Bind keys to macros on the Keys page: macro keys M1–M5 (D0 A2 0n) or "
        "any key via the 0x71 'macro slot' type.</i>");
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    a->macro_total = 1;
    gtk_widget_show_all(box);
    return box;
}
