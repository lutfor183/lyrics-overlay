#pragma once
#include <glib.h>

/* ---- log ---- */
void app_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---- lrc ---- */
typedef struct { double t; char *text; } LyLine;
typedef struct { LyLine *v; int n; } LyLines;
LyLines parse_lrc(const char *text);
void    lines_free(LyLines *ls);
int     line_at(const LyLines *ls, double pos); /* -1 if none yet */

/* ---- settings ---- */
typedef struct {
  char *font_family, *text_color, *bg_color, *bg_mode, *bg_custom_color;
  char *anchor, *source;
  int font_size, width, margin, poll_ms, lines, autohide_timeout, sync_offset_ms;
  int resync_ms; /* authoritative MPRIS re-read cadence while playing */
  double secondary_scale;
  gboolean text_shadow, cache, autohide, keep_above;
} Settings;
Settings *settings_load_from(const char *etc_ini, const char *user_ini);
Settings *settings_load_defaults(void);
void      settings_save_user(const Settings *s, const char *user_ini);
void      settings_free(Settings *s);
const char *user_ini_path(void);
const char *cache_dir(void);
