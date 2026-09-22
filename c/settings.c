#include "common.h"
#include <stdlib.h>
#include <string.h>

const char *user_ini_path(void) {
  static char p[1024]; static int init = 0;
  if (!init) { g_snprintf(p, sizeof p, "%s/.config/lyrics-overlay/settings.ini", g_get_home_dir()); init = 1; }
  return p;
}
const char *cache_dir(void) {
  static char p[1024]; static int init = 0;
  if (!init) { g_snprintf(p, sizeof p, "%s/.cache/lyrics-overlay", g_get_home_dir()); init = 1; }
  return p;
}

static gboolean get_bool(GKeyFile *k, const char *sec, const char *key, gboolean dflt) {
  GError *e = NULL;
  char *v = g_key_file_get_string(k, sec, key, &e);
  if (e) { g_error_free(e); return dflt; }
  gboolean r = dflt;
  if (v) {
    char *l = g_ascii_strdown(v, -1);
    r = !strcmp(l, "1") || !strcmp(l, "true") || !strcmp(l, "yes") || !strcmp(l, "on");
    g_free(l); g_free(v);
  }
  return r;
}
static int get_int(GKeyFile *k, const char *sec, const char *key, int dflt) {
  GError *e = NULL;
  int v = g_key_file_get_integer(k, sec, key, &e);
  if (e) { g_error_free(e); return dflt; }
  return v;
}
static double get_dbl(GKeyFile *k, const char *sec, const char *key, double dflt) {
  GError *e = NULL;
  double v = g_key_file_get_double(k, sec, key, &e);
  if (e) { g_error_free(e); return dflt; }
  return v;
}
static char *get_str(GKeyFile *k, const char *sec, const char *key, const char *dflt) {
  GError *e = NULL;
  char *v = g_key_file_get_string(k, sec, key, &e);
  if (e) { g_error_free(e); return g_strdup(dflt); }
  return v ? v : g_strdup(dflt);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

Settings *settings_load_from(const char *etc_ini, const char *u_ini) {
  GKeyFile *k = g_key_file_new();
  GError *e = NULL;
  if (etc_ini) { g_key_file_load_from_file(k, etc_ini, G_KEY_FILE_NONE, NULL); }
  if (u_ini) { g_key_file_load_from_file(k, u_ini, G_KEY_FILE_NONE, NULL); }
  (void)e;
  Settings *s = g_new0(Settings, 1);
  s->font_family = get_str(k, "display", "font_family", "Sans");
  s->font_size = get_int(k, "display", "font_size", 22);
  s->secondary_scale = get_dbl(k, "display", "secondary_scale", 0.75);
  s->text_color = get_str(k, "display", "text_color", "#ffffff");
  s->bg_color = get_str(k, "display", "bg_color", "#00000000");
  s->bg_mode = get_str(k, "display", "bg_mode", "transparent");
  s->bg_custom_color = get_str(k, "display", "bg_custom_color", "#00000000");
  s->text_shadow = get_bool(k, "display", "text_shadow", TRUE);
  s->width = get_int(k, "display", "width", 480);
  s->anchor = get_str(k, "display", "anchor", "bottom");
  s->margin = get_int(k, "display", "margin", 48);
  s->poll_ms = clampi(get_int(k, "display", "poll_ms", 100), 50, 1000);
  s->lines = clampi(get_int(k, "display", "lines", 1), 1, 3);
  s->cache = get_bool(k, "display", "cache", TRUE);
  s->autohide = get_bool(k, "display", "autohide", TRUE);
  s->autohide_timeout = get_int(k, "display", "autohide_timeout", 5);
  if (s->autohide_timeout < 1) s->autohide_timeout = 1;
  s->keep_above = get_bool(k, "display", "keep_above", TRUE);
  s->source = get_str(k, "display", "source", "auto");
  s->sync_offset_ms = clampi(get_int(k, "display", "sync_offset_ms", 350), -2000, 2000);
  s->resync_ms = get_int(k, "display", "position_resync_ms", 5000);
  if (s->resync_ms < 1000) s->resync_ms = 1000;
  if (s->resync_ms > 30000) s->resync_ms = 30000;
  g_key_file_free(k);
  return s;
}
Settings *settings_load_defaults(void) {
  return settings_load_from("/etc/lyrics-overlay/settings.ini", user_ini_path());
}
void settings_save_user(const Settings *s, const char *u_ini) {
  GKeyFile *k = g_key_file_new();
  g_key_file_set_string(k, "display", "font_family", s->font_family);
  g_key_file_set_integer(k, "display", "font_size", s->font_size);
  g_key_file_set_double(k, "display", "secondary_scale", s->secondary_scale);
  g_key_file_set_string(k, "display", "text_color", s->text_color);
  g_key_file_set_string(k, "display", "bg_color", s->bg_color);
  g_key_file_set_string(k, "display", "bg_mode", s->bg_mode);
  g_key_file_set_string(k, "display", "bg_custom_color", s->bg_custom_color);
  g_key_file_set_boolean(k, "display", "text_shadow", s->text_shadow);
  g_key_file_set_integer(k, "display", "width", s->width);
  g_key_file_set_string(k, "display", "anchor", s->anchor);
  g_key_file_set_integer(k, "display", "margin", s->margin);
  g_key_file_set_integer(k, "display", "poll_ms", s->poll_ms);
  g_key_file_set_integer(k, "display", "lines", s->lines);
  g_key_file_set_boolean(k, "display", "cache", s->cache);
  g_key_file_set_boolean(k, "display", "autohide", s->autohide);
  g_key_file_set_integer(k, "display", "autohide_timeout", s->autohide_timeout);
  g_key_file_set_boolean(k, "display", "keep_above", s->keep_above);
  g_key_file_set_string(k, "display", "source", s->source);
  g_key_file_set_integer(k, "display", "sync_offset_ms", s->sync_offset_ms);
  g_key_file_set_integer(k, "display", "position_resync_ms", s->resync_ms);
  char *dir = g_path_get_dirname(u_ini);
  g_mkdir_with_parents(dir, 0755); g_free(dir);
  g_key_file_save_to_file(k, u_ini, NULL);
  g_key_file_free(k);
}
void settings_free(Settings *s) {
  if (!s) return;
  g_free(s->font_family); g_free(s->text_color); g_free(s->bg_color);
  g_free(s->bg_mode); g_free(s->bg_custom_color); g_free(s->anchor); g_free(s->source);
  g_free(s);
}
