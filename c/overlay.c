#include "overlay.h"
#include <string.h>

struct Overlay {
  GtkWidget *win, *root, *prev, *cur, *next;
  GtkCssProvider *css;
  GtkWidget *menu;
  Settings *s;
  App *cb;
  guint save_id, hide_id;
  gboolean playing, single, settings_queued;
};

static char *hex_alpha(const char *rrggbbaa) {
  const char *h = (rrggbbaa && *rrggbbaa == '#') ? rrggbbaa + 1 : (rrggbbaa ? rrggbbaa : "");
  if (strlen(h) == 6) {
    char *r = g_strdup_printf("#%s", h); return r;
  }
  if (strlen(h) != 8) return g_strdup("rgba(0,0,0,0.5)");
  char *e = NULL;
  long v = strtol(h, &e, 16);
  if (!e || *e) return g_strdup("rgba(0,0,0,0.5)");
  int r = (v >> 24) & 255, g = (v >> 16) & 255, b = (v >> 8) & 255, a = v & 255;
  if (a >= 255) return g_strdup_printf("#%06lx", v >> 8);
  return g_strdup_printf("rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0);
}
static const char *resolved_bg(Settings *s) {
  if (!strcmp(s->bg_mode, "transparent")) return "#00000000";
  if (!strcmp(s->bg_mode, "minimal_dark")) return "#000000B3";
  if (!strcmp(s->bg_mode, "light")) return "#FFFFFFCC";
  if (!strcmp(s->bg_mode, "custom")) return s->bg_custom_color;
  return s->bg_color;
}
static void apply_css(Overlay *o) {
  Settings *s = o->s;
  int side = (int)(s->font_size * s->secondary_scale);
  if (side < 10) side = 10;
  const char *shadow = s->text_shadow
      ? "text-shadow: 0 1px 3px rgba(0,0,0,0.9), 0 0 12px rgba(0,0,0,0.7);"
      : "text-shadow: none;";
  char *bg = hex_alpha(resolved_bg(s));
  char *css = g_strdup_printf(
      ".lyrics-win { background-color: transparent; border: none; box-shadow: none; }"
      ".lyrics-root { background-color: %s; border-radius: 10px; }"
      ".lyrics-current { font-family: %s; font-size: %dpx; font-weight: bold; color: %s; %s }"
      ".lyrics-side { font-family: %s; font-size: %dpx; color: %s; opacity: 0.65; %s }",
      bg, s->font_family, s->font_size, s->text_color, shadow,
      s->font_family, side, s->text_color, shadow);
  g_free(bg);
  gtk_css_provider_load_from_string(o->css, css);
  g_free(css);
}
static GtkWidget *mklabel(const char *cls, gboolean one) {
  GtkWidget *lb = gtk_label_new("");
  gtk_widget_add_css_class(lb, cls);
  gtk_label_set_justify(GTK_LABEL(lb), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(lb, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(lb, GTK_ALIGN_CENTER);
  (void)one;
  /* every label wraps on words: long lines grow taller instead of
     degrading into "..." */
  gtk_label_set_wrap(GTK_LABEL(lb), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(lb), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_single_line_mode(GTK_LABEL(lb), FALSE);
  gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_NONE);
  gtk_label_set_lines(GTK_LABEL(lb), -1);
  return lb;
}
static void apply_mode(Overlay *o) {
  gtk_widget_set_visible(o->prev, !o->single);
  gtk_widget_set_visible(o->next, !o->single);
  if (o->single) {
    gtk_label_set_text(GTK_LABEL(o->prev), "");
    gtk_label_set_text(GTK_LABEL(o->next), "");
  }
  gtk_widget_set_margin_top(o->root, o->single ? 4 : 8);
  gtk_widget_set_margin_bottom(o->root, o->single ? 4 : 8);
}
static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud) {
  Overlay *o = ud;
  GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(o->win));
  GdkToplevel *tl = (surf && GDK_IS_TOPLEVEL(surf)) ? GDK_TOPLEVEL(surf) : NULL;
  if (!tl) return;
  GdkDevice *dev = gtk_gesture_get_device(GTK_GESTURE(g));
  gdk_toplevel_begin_move(tl, dev, 1, x, y, GDK_CURRENT_TIME);
  app_log("drag: move started");
}
static void show_settings(Overlay *o);
static void show_settings_later(GtkButton *b, gpointer ud);
static void on_menu_closed(GtkPopover *pop, gpointer ud);
static void menu_row(GtkWidget *box, const char *icon, const char *label,
                       GCallback fn, gpointer ud, Overlay *o) {
  GtkWidget *b = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *im = gtk_image_new_from_icon_name(icon);
  gtk_widget_set_valign(im, GTK_ALIGN_CENTER);
  GtkWidget *lb = gtk_label_new(label);
  gtk_widget_set_halign(lb, GTK_ALIGN_START);
  gtk_widget_set_hexpand(lb, TRUE);
  gtk_box_append(GTK_BOX(row), im);
  gtk_box_append(GTK_BOX(row), lb);
  gtk_button_set_child(GTK_BUTTON(b), row);
  /* dismiss first so the action's window (e.g. Settings) is never trapped
     under the menu's grab */
  g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), o->menu);
  g_signal_connect_swapped(b, "clicked", fn, ud);
  gtk_box_append(GTK_BOX(box), b);
}
static void on_right(GtkGestureClick *g, int n, double x, double y, gpointer ud) {
  (void)g; (void)n;
  Overlay *o = ud;
  if (o->menu) {
    gtk_popover_popdown(GTK_POPOVER(o->menu));
    gtk_widget_unparent(o->menu);
    o->menu = NULL;
  }
  GtkWidget *pop = gtk_popover_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 10); gtk_widget_set_margin_end(box, 10);
  /* header: what is on screen right now */
  const char *cur = gtk_label_get_text(GTK_LABEL(o->cur));
  char *hdr;
  if (cur && *cur && strcmp(cur, "\u266A") != 0) {
    if (g_utf8_strlen(cur, -1) > 40) {
      char *sub = g_utf8_substring(cur, 0, 38);
      hdr = g_strdup_printf("\u266A %s\u2026", sub);
      g_free(sub);
    } else {
      hdr = g_strdup_printf("\u266A %s", cur);
    }
  } else {
    hdr = g_strdup("Lyrics Overlay");
  }
  GtkWidget *hl = gtk_label_new(NULL);
  char *mh = g_markup_printf_escaped("<b>%s</b>", hdr);
  gtk_label_set_markup(GTK_LABEL(hl), mh);
  g_free(mh); g_free(hdr);
  gtk_widget_set_halign(hl, GTK_ALIGN_START);
  gtk_widget_set_sensitive(hl, FALSE);
  gtk_box_append(GTK_BOX(box), hl);
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(box), sep);
  menu_row(box, "view-refresh", "Refresh lyrics", G_CALLBACK(app_refresh), o->cb, o);
  menu_row(box, "preferences-system", "Settings\u2026", G_CALLBACK(show_settings_later), o, o);
  menu_row(box, "application-exit", "Quit", G_CALLBACK(app_quit), o->cb, o);
  gtk_popover_set_child(GTK_POPOVER(pop), box);
  gtk_widget_set_parent(pop, o->win);
  GdkRectangle r = { (int)x, (int)y, 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
  gtk_widget_set_size_request(pop, 250, -1);
  g_signal_connect(pop, "closed", G_CALLBACK(on_menu_closed), o);
  gtk_popover_popup(GTK_POPOVER(pop));
  o->menu = pop;
  app_log("menu: popped");
}
static void show_settings_later(GtkButton *b, gpointer ud) {
  (void)b;
  /* run after the menu fully dismisses so its grab is gone */
  ((Overlay *)ud)->settings_queued = TRUE;
}
static void on_menu_closed(GtkPopover *pop, gpointer ud) {
  Overlay *o = ud;
  gboolean open_settings = o->settings_queued;
  o->settings_queued = FALSE;
  if (o->menu == (GtkWidget *)pop) o->menu = NULL;
  gtk_widget_unparent(GTK_WIDGET(pop));
  if (open_settings) show_settings(o);
}
Overlay *overlay_new(GtkApplication *app, Settings *s, App *cb) {
  Overlay *o = g_new0(Overlay, 1);
  o->s = s; o->cb = cb;
  o->single = (s->lines == 1);
  o->win = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(o->win), "Lyrics Overlay");
  gtk_widget_add_css_class(o->win, "lyrics-win");
  gtk_window_set_decorated(GTK_WINDOW(o->win), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(o->win), TRUE);
  o->css = gtk_css_provider_new();
  apply_css(o);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(o->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  o->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(o->root, "lyrics-root");
  gtk_widget_set_margin_top(o->root, 8); gtk_widget_set_margin_bottom(o->root, 8);
  gtk_widget_set_margin_start(o->root, 14); gtk_widget_set_margin_end(o->root, 14);
  gtk_window_set_child(GTK_WINDOW(o->win), o->root);
  o->prev = mklabel("lyrics-side", FALSE);
  o->cur = mklabel("lyrics-current", TRUE);
  o->next = mklabel("lyrics-side", FALSE);
  gtk_label_set_text(GTK_LABEL(o->cur), "\u266A");
  gtk_box_append(GTK_BOX(o->root), o->prev);
  gtk_box_append(GTK_BOX(o->root), o->cur);
  gtk_box_append(GTK_BOX(o->root), o->next);
  apply_mode(o);
  GtkGesture *drag = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), o);
  gtk_widget_add_controller(o->win, GTK_EVENT_CONTROLLER(drag));
  GtkGesture *rc = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), 3);
  g_signal_connect(rc, "pressed", G_CALLBACK(on_right), o);
  gtk_widget_add_controller(o->win, GTK_EVENT_CONTROLLER(rc));
  app_log("window created single_line=%d", o->single);
  return o;
}
GtkWindow *overlay_window(Overlay *o) { return GTK_WINDOW(o->win); }
void overlay_refresh(Overlay *o, Settings *s) {
  o->s = s;
  o->single = (s->lines == 1);
  apply_css(o);
  apply_mode(o);
  gtk_widget_queue_resize(o->win);
  if (!s->autohide && !gtk_widget_get_visible(o->win))
    gtk_window_present(GTK_WINDOW(o->win));
}
static void refit(Overlay *o) {
  int minw = 0, natw = 0;
  gtk_widget_measure(o->cur, GTK_ORIENTATION_HORIZONTAL, -1, &minw, &natw, NULL, NULL);
  int cap = o->s->width > 200 ? o->s->width : 200;
  int w = natw + 36;
  if (w > cap) w = cap;
  /* height measured AT the final width: a long line wraps onto more rows
     and the window grows instead of clipping it */
  int minh = 0, nath = 0;
  gtk_widget_measure(o->root, GTK_ORIENTATION_VERTICAL, w, &minh, &nath, NULL, NULL);
  gtk_window_set_default_size(GTK_WINDOW(o->win), w, nath);
}
static void present(Overlay *o) {
  if (!gtk_widget_get_visible(o->win))
    gtk_window_present(GTK_WINDOW(o->win));
}
static void set_cur(Overlay *o, const char *t) {
  char *e = g_markup_escape_text(t && *t ? t : "\u266A", -1);
  char *m = g_strdup_printf("<span>%s</span>", e);
  gtk_label_set_markup(GTK_LABEL(o->cur), m);
  g_free(e); g_free(m);
}
void overlay_show_lyrics(Overlay *o, const char *prev, const char *cur, const char *next) {
  if (o->single) { overlay_show_single(o, cur); return; }
  apply_mode(o);
  char *e;
  e = g_markup_escape_text(prev && *prev ? prev : "", -1);
  char *mp = g_strdup_printf("<span>%s</span>", e); g_free(e);
  gtk_label_set_markup(GTK_LABEL(o->prev), *prev ? mp : "");
  g_free(mp);
  set_cur(o, cur);
  e = g_markup_escape_text(next && *next ? next : "", -1);
  char *mn = g_strdup_printf("<span>%s</span>", e); g_free(e);
  gtk_label_set_markup(GTK_LABEL(o->next), *next ? mn : "");
  g_free(mn);
  refit(o); present(o);
}
void overlay_show_single(Overlay *o, const char *cur) {
  apply_mode(o);
  set_cur(o, cur);
  refit(o); present(o);
}
void overlay_show_status(Overlay *o, const char *text) {
  apply_mode(o);
  gtk_label_set_text(GTK_LABEL(o->cur), text);
  present(o);
}
char *overlay_dump(Overlay *o) {
  return g_strdup_printf("visible=%d size=%dx%d single=%d cur=%.40s",
      gtk_widget_get_visible(o->win),
      gtk_widget_get_width(o->win), gtk_widget_get_height(o->win),
      o->single, gtk_label_get_text(GTK_LABEL(o->cur)));
}
/* ---- autohide ---- */
static gboolean do_hide(gpointer ud) {
  Overlay *o = ud; o->hide_id = 0;
  if (o->s->autohide && !o->playing) {
    gtk_widget_set_visible(o->win, FALSE);
    app_log("autohide: hidden");
  }
  return G_SOURCE_REMOVE;
}
void overlay_set_playing(Overlay *o, gboolean playing) {
  o->playing = playing;
  if (playing) {
    if (o->hide_id) { g_source_remove(o->hide_id); o->hide_id = 0; }
    if (!gtk_widget_get_visible(o->win))
      gtk_window_present(GTK_WINDOW(o->win));
  } else if (o->s->autohide && !o->hide_id) {
    int t = o->s->autohide_timeout > 0 ? o->s->autohide_timeout : 1;
    o->hide_id = g_timeout_add_seconds(t, do_hide, o);
  }
}
/* ---- settings dialog ---- */
typedef struct {
  Overlay *o;
  GtkWidget *dlg, *lines, *src, *font, *fsize, *scale, *color, *bg,
            *width, *anchor, *margin, *poll, *autohide, *timeout,
            *bgcolor,
            *cache, *sync;
} Dlg;
static void dlg_row(GtkWidget *grid, int row, const char *label, GtkWidget *w,
                    gboolean fill) {
  GtkWidget *l = gtk_label_new(label);
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(l, 170, -1);
  gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
  gtk_widget_set_hexpand(w, TRUE);
  gtk_widget_set_halign(w, fill ? GTK_ALIGN_FILL : GTK_ALIGN_END);
  if (fill) gtk_widget_set_size_request(w, 220, -1);
  gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
}
static int parse_rrggbbaa(const char *s, GdkRGBA *out) {
  if (!s) return 0;
  if (*s == '#') s++;
  int n = strlen(s);
  if (n != 6 && n != 8) return 0;
  char *e = NULL;
  unsigned long v = strtoul(s, &e, 16);
  if (!e || *e) return 0;
  if (n == 6) v = (v << 8) | 0xFF;
  out->red = ((v >> 24) & 255) / 255.0;
  out->green = ((v >> 16) & 255) / 255.0;
  out->blue = ((v >> 8) & 255) / 255.0;
  out->alpha = (v & 255) / 255.0;
  return 1;
}
static char *rgba_hex(const GdkRGBA *c) {
  int v[4];
  v[0] = (int)(c->red * 255 + 0.5); v[1] = (int)(c->green * 255 + 0.5);
  v[2] = (int)(c->blue * 255 + 0.5); v[3] = (int)(c->alpha * 255 + 0.5);
  for (int i = 0; i < 4; i++) { if (v[i] < 0) v[i] = 0; if (v[i] > 255) v[i] = 255; }
  return g_strdup_printf("#%02X%02X%02X%02X", v[0], v[1], v[2], v[3]);
}
static void bg_changed(GtkComboBox *c, gpointer ud) {
  Dlg *d = ud;
  const char *v = gtk_combo_box_get_active_id(c);
  gtk_widget_set_sensitive(d->bgcolor, v && !strcmp(v, "custom"));
}
static GtkWidget *combo(const char *ids[][2], int n, const char *active) {
  GtkWidget *c = gtk_combo_box_text_new();
  for (int i = 0; i < n; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), ids[i][0], ids[i][1]);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), active);
  return c;
}
static void dlg_apply(GtkButton *b, gpointer ud) {
  (void)b;
  Dlg *d = ud; Overlay *o = d->o; Settings *s = o->s;
  const char *v;
  v = gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->lines));
  s->lines = v && !strcmp(v, "3") ? 3 : 1;
  g_free(s->font_family);
  s->font_family = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->font)));
  if (!*s->font_family) { g_free(s->font_family); s->font_family = g_strdup("Sans"); }
  s->font_size = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->fsize));
  s->secondary_scale = gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->scale));
  GdkRGBA rgba;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(d->color), &rgba);
  g_free(s->text_color);
  s->text_color = gdk_rgba_to_string(&rgba);
  g_free(s->bg_mode);
  s->bg_mode = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->bg)) ?: "transparent");
  {
    GdkRGBA br;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(d->bgcolor), &br);
    g_free(s->bg_custom_color);
    s->bg_custom_color = rgba_hex(&br);
  }
  s->width = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->width));
  g_free(s->anchor);
  s->anchor = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->anchor)) ?: "bottom");
  s->margin = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->margin));
  s->resync_ms = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->poll));
  if (s->resync_ms < 1000) s->resync_ms = 1000;
  if (s->resync_ms > 30000) s->resync_ms = 30000;
  s->autohide = gtk_switch_get_active(GTK_SWITCH(d->autohide));
  s->autohide_timeout = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->timeout));
  if (s->autohide_timeout < 1) s->autohide_timeout = 1;
  s->cache = gtk_switch_get_active(GTK_SWITCH(d->cache));
  s->sync_offset_ms = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->sync));
  if (s->sync_offset_ms < -2000) s->sync_offset_ms = -2000;
  if (s->sync_offset_ms > 2000) s->sync_offset_ms = 2000;
  g_free(s->source);
  s->source = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->src)) ?: "auto");
  s->text_shadow = TRUE;
  settings_save_user(s, user_ini_path());
  overlay_refresh(o, s);
  app_rearm(o->cb);
  app_log("settings applied lines=%d size=%d src=%s", s->lines, s->font_size, s->source);
  gtk_window_destroy(GTK_WINDOW(d->dlg));
  /* d is freed by the destroy handler */
}
static void show_settings(Overlay *o);
void overlay_open_settings(Overlay *o) { show_settings(o); }
static void show_settings(Overlay *o) {
  app_log("settings dialog opened");
  Settings *s = o->s;
  Dlg *d = g_new0(Dlg, 1); d->o = o;
  d->dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(d->dlg), "Lyrics Overlay Settings");
  gtk_window_set_transient_for(GTK_WINDOW(d->dlg), GTK_WINDOW(o->win));
  gtk_window_set_modal(GTK_WINDOW(d->dlg), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(d->dlg), 470, 430);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
  gtk_window_set_child(GTK_WINDOW(d->dlg), box);
  GtkWidget *nb = gtk_notebook_new();
  gtk_widget_set_vexpand(nb, TRUE);
  gtk_box_append(GTK_BOX(box), nb);
  GtkWidget *ga = gtk_grid_new(), *gb = gtk_grid_new(), *gc = gtk_grid_new();
  GtkWidget *gd = gtk_grid_new(), *ge = gtk_grid_new();
  const char *_tabs[] = { "Lyrics", "Appearance", "Window", "Updates", "Behavior" };
  GtkWidget *_grids[] = { ga, gb, gc, gd, ge };
  for (int _i = 0; _i < 5; _i++) {
    gtk_grid_set_row_spacing(GTK_GRID(_grids[_i]), 8);
    gtk_grid_set_column_spacing(GTK_GRID(_grids[_i]), 12);
    gtk_widget_set_margin_top(_grids[_i], 10);
    gtk_widget_set_margin_start(_grids[_i], 6);
    gtk_widget_set_margin_end(_grids[_i], 6);
    GtkWidget *_pg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(_pg), _grids[_i]);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), _pg, gtk_label_new(_tabs[_i]));
  }
  const char *L[][2] = { {"1", "Single line"}, {"3", "3-line (prev/cur/next)"} };
  d->lines = combo(L, 2, s->lines == 3 ? "3" : "1");
  dlg_row(ga, 0, "Lines", d->lines, 1);
  const char *S[][2] = { {"auto", "Auto (best match)"}, {"local", "My .lrc files"},
    {"limusic", "limusic match"}, {"mpris", "From player"},
    {"boidu", "Boidu"}, {"lrclib", "LRCLIB"},
    {"netease", "Netease"}, {"qq", "QQ"}, {"kugou", "Kugou"} };
  d->src = combo(S, 9, s->source ? s->source : "auto");
  dlg_row(ga, 1, "Lyrics source", d->src, 1);
  d->font = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(d->font), s->font_family);
  dlg_row(gb, 0, "Font family", d->font, 1);
  d->fsize = gtk_spin_button_new_with_range(10, 72, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->fsize), s->font_size);
  dlg_row(gb, 1, "Font size", d->fsize, 1);
  d->scale = gtk_spin_button_new_with_range(0.3, 2.0, 0.05);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->scale), s->secondary_scale);
  dlg_row(gb, 2, "Secondary scale", d->scale, 1);
  d->color = gtk_color_button_new();
  GdkRGBA rgba;
  if (gdk_rgba_parse(&rgba, s->text_color)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(d->color), &rgba);
  dlg_row(gb, 3, "Text color", d->color, 0);
  const char *B[][2] = { {"transparent", "Fully transparent"}, {"minimal_dark", "Minimal dark shade"},
    {"light", "Light shade"}, {"custom", "Custom color"} };
  d->bg = combo(B, 4, s->bg_mode);
  dlg_row(gb, 4, "Background", d->bg, 1);
  d->bgcolor = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(d->bgcolor), TRUE);
  {
    GdkRGBA br = { 0, 0, 0, 0 };
    if (!parse_rrggbbaa(s->bg_custom_color, &br))
      gdk_rgba_parse(&br, "#00000000");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(d->bgcolor), &br);
  }
  gtk_widget_set_tooltip_text(d->bgcolor,
      "Used when Background is Custom color (alpha = transparency)");
  dlg_row(gb, 5, "Custom background", d->bgcolor, 0);
  {
    const char *bv = gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->bg));
    gtk_widget_set_sensitive(d->bgcolor, bv && !strcmp(bv, "custom"));
  }
  g_signal_connect(d->bg, "changed", G_CALLBACK(bg_changed), d);
  d->width = gtk_spin_button_new_with_range(200, 1200, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->width), s->width);
  dlg_row(gc, 0, "Window width", d->width, 1);
  const char *A[][2] = { {"bottom", "Bottom"}, {"top", "Top"} };
  d->anchor = combo(A, 2, s->anchor);
  dlg_row(gc, 1, "Anchor", d->anchor, 1);
  d->margin = gtk_spin_button_new_with_range(0, 200, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->margin), s->margin);
  dlg_row(gc, 2, "Margin", d->margin, 1);
  d->poll = gtk_spin_button_new_with_range(1000, 30000, 1000);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->poll), s->resync_ms);
  dlg_row(gd, 0, "Position resync (ms)", d->poll, 1);
  d->autohide = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(d->autohide), s->autohide);
  dlg_row(ge, 0, "Autohide", d->autohide, 0);
  d->timeout = gtk_spin_button_new_with_range(1, 30, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->timeout), s->autohide_timeout);
  dlg_row(ge, 1, "Autohide timeout (s)", d->timeout, 1);
  d->cache = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(d->cache), s->cache);
  dlg_row(ge, 2, "Cache lyrics", d->cache, 0);
  d->sync = gtk_spin_button_new_with_range(-2000, 2000, 50);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->sync), s->sync_offset_ms);
  dlg_row(gd, 1, "Show early (ms)", d->sync, 1);
  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), btns);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), d->dlg);
  gtk_box_append(GTK_BOX(btns), cancel);
  GtkWidget *apply = gtk_button_new_with_label("Apply");
  g_signal_connect(apply, "clicked", G_CALLBACK(dlg_apply), d);
  gtk_box_append(GTK_BOX(btns), apply);
  g_signal_connect_swapped(d->dlg, "destroy", G_CALLBACK(g_free), d);
  gtk_window_present(GTK_WINDOW(d->dlg));
}
