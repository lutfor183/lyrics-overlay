#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>
#include "common.h"
#include "overlay.h"
#include "mpris.h"
#include "lyrics.h"

static void check_state(void);
static void poke_later(guint ms);
static gboolean lyric_cb(gpointer ud);
static double pos_now(void);
static void paint_idx(const char *key, int idx);
static void schedule_next(void);

struct App {
  GtkApplication *gapp;
  Overlay *win;
  Tracker *tracker;
  Settings *s;
  LyLines lines;
  Player cur; gboolean have_cur;
  char *track_key, *loading_key, *shown_key;
  int shown_idx, fetch_gen;
  guint lyric_timer, resync_timer;
  double anchor_pos, anchor_mono; /* last authoritative position + when */
  gboolean playing;
  double ini_mtime;
};
static App G = { 0 };

void app_quit(App *a) { (void)a; g_application_quit(G_APPLICATION(G.gapp)); }
static void check_state(void);
static void poke_later(guint ms);
void app_rearm(App *a) {
  (void)a;
  if (G.tracker && G.s) tracker_set_resync_ms(G.tracker, G.s->resync_ms);
  poke_later(100); /* settings may change cadence/sources */
}
void app_refresh(App *a) {
  (void)a;
  g_free(G.track_key); G.track_key = g_strdup("");
  g_free(G.shown_key); G.shown_key = g_strdup(""); G.shown_idx = -99;
  if (G.tracker) tracker_request_resync(G.tracker);
  poke_later(100);
}
/* D-Bus signals land here (via mpris.c): re-check soon, never block */
void mpris_poke(void) {
  if (G.tracker) tracker_request_resync(G.tracker);
  poke_later(400);
}

/* ---------- fetch thread ---------- */
typedef struct {
  char *artist, *title, *url, *album, *key, *bus, *meta, *pin;
  double dur; gboolean cache, display; int gen;
} FetchArgs;
typedef struct {
  char *key, *text, *src, *artist, *title;
  double dur; gboolean cache, display; int gen;
} DoneArgs;
static gboolean fetch_done_cb(gpointer ud) {
  DoneArgs *d = ud;
  if (d->gen != G.fetch_gen) goto out; /* stale song */
  if (G.loading_key && !strcmp(G.loading_key, d->key)) {
    g_free(G.loading_key); G.loading_key = g_strdup("");
  }
  if (!d->display) goto out;
  if (d->cache && d->text)
    write_cache(d->artist, d->title, d->text, d->dur);
  if (G.track_key && !strcmp(G.track_key, d->key) && G.lines.n)
    goto out; /* already painting; keep timestamps */
  g_free(G.shown_key); G.shown_key = g_strdup(""); G.shown_idx = -99;
  lines_free(&G.lines);
  if (d->text) G.lines = parse_lrc(d->text);
  app_log("fetch-done %s src=%s lines=%d", d->key,
          d->src ? d->src : "-", G.lines.n);
  g_free(G.track_key); G.track_key = g_strdup(d->key);
  if (G.lines.n) {
    paint_idx(d->key, line_at(&G.lines, pos_now()));
    schedule_next();
  } else {
    if (d->cache) write_miss(d->artist, d->title, d->dur);
    if (G.win) overlay_show_status(G.win, "No lyrics found");
  }
out:
  g_free(d->key); g_free(d->text); g_free(d->src);
  g_free(d->artist); g_free(d->title); g_free(d);
  return G_SOURCE_REMOVE;
}
static gpointer fetch_thread(gpointer ud) {
  FetchArgs *f = ud;
  char *meta_lyrics = f->meta ? f->meta : g_strdup("");
  f->meta = NULL;
  char *pinned = (f->pin && *f->pin) ? f->pin : "auto";
  f->pin = NULL;
  const char *srcs[] = { "local", "limusic", "mpris", "boidu", "lrclib", NULL };
  int use_pin = 0;
  for (int i = 0; srcs[i]; i++)
    if (!strcmp(pinned, srcs[i])) use_pin = 1;
  char *text = NULL; const char *where = NULL;
  if (use_pin) {
    char *one = run_source(pinned, f->artist, f->title, f->url, f->dur,
                           f->album, meta_lyrics);
    if (one && is_synced_lrc(one)) { text = one; where = pinned; }
    else g_free(one);
    app_log("sources %s pinned=%s hit=%d", f->key, pinned, text != NULL);
  } else {
    const char *w = NULL;
    text = fetch_first_synced(f->artist, f->title, f->url, f->dur,
                              f->album, meta_lyrics, &w);
    where = w;
    int nl = 0;
    if (text) for (const char *c = text; *c; c++) if (*c == '\n') nl++;
    app_log("sources %s win=%s lines=%d", f->key, w ? w : "-", nl);
  }
  DoneArgs *d = g_new0(DoneArgs, 1);
  d->key = f->key; d->text = text; d->src = g_strdup(where ? where : "-");
  d->artist = f->artist; d->title = f->title; d->dur = f->dur;
  d->cache = f->cache; d->display = f->display; d->gen = f->gen;
  g_free(f->url); g_free(f->album); g_free(f->bus); g_free(f->pin); g_free(f);
  g_free(meta_lyrics);
  g_idle_add(fetch_done_cb, d);
  return NULL;
}

/* ---------- scheduler: no polling; each lyric gets its own timer ---------- */
static double mono_now(void) { return g_get_monotonic_time() / 1e6; }
/* authoritative position, interpolated locally between slow resyncs */
static double pos_now(void) {
  double p = G.anchor_pos + (mono_now() - G.anchor_mono);
  if (G.s) p += G.s->sync_offset_ms / 1000.0;
  return p < 0 ? 0 : p;
}
static void cancel_timers(void) {
  if (G.lyric_timer) { g_source_remove(G.lyric_timer); G.lyric_timer = 0; }
  if (G.resync_timer) { g_source_remove(G.resync_timer); G.resync_timer = 0; }
}
static void paint_idx(const char *key, int idx) {
  if (!G.win) return;
  app_log("line %s idx=%d %.50s", key, idx,
          (idx >= 0 && idx < G.lines.n) ? G.lines.v[idx].text : "-");
  g_free(G.shown_key); G.shown_key = g_strdup(key); G.shown_idx = idx;
  if (idx >= 0) {
    const char *pr = idx > 0 ? G.lines.v[idx - 1].text : "";
    const char *nx = idx + 1 < G.lines.n ? G.lines.v[idx + 1].text : "";
    overlay_show_lyrics(G.win, pr, G.lines.v[idx].text, nx);
  } else {
    overlay_show_single(G.win, "\u266A");
  }
}
/* one-shot timer for exactly the next lyric timestamp: every line shows */
static void schedule_next(void) {
  if (G.lyric_timer) { g_source_remove(G.lyric_timer); G.lyric_timer = 0; }
  if (!G.playing || !G.lines.n || G.shown_idx + 1 >= G.lines.n) return;
  double delay = G.lines.v[G.shown_idx + 1].t - pos_now();
  if (delay <= 0.005) {
    paint_idx(G.track_key, G.shown_idx + 1);
    schedule_next();
    return;
  }
  G.lyric_timer = g_timeout_add((guint)(delay * 1000.0), lyric_cb, NULL);
}
static gboolean lyric_cb(gpointer ud) {
  (void)ud;
  G.lyric_timer = 0;
  if (!G.playing || !G.lines.n) return G_SOURCE_REMOVE;
  /* Every line gets shown: chain-paint each missed line after a transient
     stall. Only a real jump (seek/suspend, >5 lines) skips ahead. */
  int idx = line_at(&G.lines, pos_now());
  if (idx - G.shown_idx > 5) {
    paint_idx(G.track_key, idx);
  } else {
    while (G.shown_idx < idx) paint_idx(G.track_key, G.shown_idx + 1);
  }
  schedule_next();
  return G_SOURCE_REMOVE;
}
static guint poke_pending = 0;
static gboolean check_cb(gpointer ud) {
  (void)ud;
  poke_pending = 0;
  check_state();
  return G_SOURCE_REMOVE;
}
static void poke_later(guint ms) {
  if (poke_pending) g_source_remove(poke_pending);
  poke_pending = g_timeout_add(ms, check_cb, NULL);
}
static gboolean resync_cb(gpointer ud) {
  (void)ud;
  if (!G.playing || !G.tracker) { G.resync_timer = 0; return G_SOURCE_REMOVE; }
  tracker_request_resync(G.tracker);
  poke_later(400); /* reconcile once the worker re-reads */
  return G_SOURCE_CONTINUE;
}
static void arm_resync(void) {
  if (G.resync_timer || !G.playing) return;
  int ms = (G.s && G.s->resync_ms >= 1000) ? G.s->resync_ms : 5000;
  G.resync_timer = g_timeout_add(ms, resync_cb, NULL);
}
static void reload_ini(void) {
  GStatBuf st;
  if (g_stat(user_ini_path(), &st) != 0 || st.st_mtime == (time_t)G.ini_mtime)
    return;
  G.ini_mtime = st.st_mtime;
  if (!G.ini_mtime) return;
  Settings *ns = settings_load_defaults();
  settings_free(G.s); G.s = ns;
  if (G.tracker) tracker_set_resync_ms(G.tracker, G.s->resync_ms);
  if (G.win) overlay_refresh(G.win, G.s);
  app_log("settings reloaded from ini");
}
static void start_track(const Player *a, const char *key) {
  g_free(G.loading_key); G.loading_key = g_strdup(key);
  G.fetch_gen++;
  int gen = G.fetch_gen;
  g_free(G.shown_key); G.shown_key = g_strdup(""); G.shown_idx = -99;
  if (G.lyric_timer) { g_source_remove(G.lyric_timer); G.lyric_timer = 0; }
  G.anchor_pos = a->pos_us / 1e6; G.anchor_mono = mono_now();
  app_log("track-change %s status=%s", key, a->status);
  if (G.win) {
    char *lbl = g_strdup_printf("\u266A %s \u2026", key);
    overlay_show_status(G.win, lbl); g_free(lbl);
  }
  double dur = a->dur_us / 1e6;
  if (G.s->cache && read_miss(a->artist, a->title, dur)) {
    app_log("miss-cache %s: known missing, no network", key);
    g_free(G.track_key); G.track_key = g_strdup(key);
    g_free(G.loading_key); G.loading_key = g_strdup("");
    G.anchor_pos = a->pos_us / 1e6; G.anchor_mono = mono_now();
    if (G.win) overlay_show_status(G.win, "No lyrics found");
    return;
  }
  char *fast = (G.s->cache) ? read_cache(a->artist, a->title, dur) : NULL;
  if (fast) {
    lines_free(&G.lines);
    G.lines = parse_lrc(fast); g_free(fast);
    g_free(G.track_key); G.track_key = g_strdup(key);
    g_free(G.loading_key); G.loading_key = g_strdup("");
    app_log("cache-hit %s lines=%d", key, G.lines.n);
    paint_idx(key, line_at(&G.lines, pos_now()));
    schedule_next();
  } else {
    FetchArgs *f = g_new0(FetchArgs, 1);
    f->artist = g_strdup(a->artist); f->title = g_strdup(a->title);
    f->url = g_strdup(a->url); f->album = g_strdup(a->album);
    f->meta = g_strdup(a->meta_lyrics ? a->meta_lyrics : "");
    f->pin = g_strdup((G.s && G.s->source) ? G.s->source : "auto");
    f->key = g_strdup(key); f->bus = g_strdup(a->bus);
    f->dur = dur; f->cache = G.s->cache; f->display = TRUE; f->gen = gen;
    g_thread_new("fetch", fetch_thread, f);
  }
}
/* the whole state machine: runs on events + slow resync, then sleeps */
static void check_state(void) {
  if (!G.tracker) return;
  reload_ini();
  Player tmp; memset(&tmp, 0, sizeof tmp);
  if (tracker_try_active(G.tracker, &tmp)) {
    tracker_copy_free(&G.cur);
    G.cur = tmp; G.have_cur = TRUE;
  }
  const Player *a = G.have_cur ? &G.cur : NULL;
  gboolean playing = a && !strcmp(a->status, "Playing");
  if (playing != G.playing) {
    G.playing = playing;
    if (G.win) overlay_set_playing(G.win, playing);
  }
  if (!a) {
    if (G.track_key && G.track_key[0]) {
      lines_free(&G.lines);
      g_free(G.track_key); G.track_key = g_strdup("");
      g_free(G.shown_key); G.shown_key = g_strdup(""); G.shown_idx = -99;
      cancel_timers();
      if (G.win) overlay_show_status(G.win, "Waiting for music \u2026");
    }
    return;
  }
  char *key = track_key(a);
  if (strcmp(key, G.track_key ? G.track_key : "") &&
      strcmp(key, G.loading_key ? G.loading_key : "")) {
    start_track(a, key);
  } else if (G.loading_key && !strcmp(G.loading_key, key)) {
    /* fetch in flight; keep status */
  } else if (G.track_key && !strcmp(G.track_key, key) && G.lines.n) {
    /* reconcile worker position with our interpolation (seek detect) */
    double live = a->pos_us / 1e6;
    double interp = G.anchor_pos + (mono_now() - G.anchor_mono);
    if (a->read_t > 0 && fabs(live - interp) > 1.5) {
      G.anchor_pos = live; G.anchor_mono = mono_now();
      int idx = line_at(&G.lines, pos_now());
      if (idx != G.shown_idx) paint_idx(key, idx);
      app_log("seek %s -> %.1f", key, live);
    } else if (a->read_t > 0) {
      G.anchor_pos = live; G.anchor_mono = mono_now();
    }
    int idx = line_at(&G.lines, pos_now());
    if (idx < G.shown_idx || idx - G.shown_idx > 5) paint_idx(key, idx);
    schedule_next();
  }
  g_free(key);
  if (G.playing) arm_resync();
  else if (G.resync_timer) { g_source_remove(G.resync_timer); G.resync_timer = 0; }
  if (!G.playing && G.lyric_timer) {
    g_source_remove(G.lyric_timer); G.lyric_timer = 0;
  }
}
/* ---------- app ---------- */
static void on_activate(GtkApplication *app, gpointer ud) {
  (void)ud;
  if (!G.win) {
    G.win = overlay_new(app, G.s, &G);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus) { G.tracker = tracker_new(bus); g_object_unref(bus); }
    else app_log("no session bus");
    overlay_show_status(G.win, "Waiting for music \u2026");
    if (G.tracker) tracker_set_resync_ms(G.tracker, G.s->resync_ms);
    poke_later(300);
  }
  gtk_window_present(overlay_window(G.win));
}
int main(int argc, char **argv) {
  gboolean write_defaults = FALSE, no_cache = FALSE, version = FALSE;
  char *font_family = NULL, *text_color = NULL, *bg_color = NULL;
  int font_size = -1, width = -1, lines = -1;
  char *anchor = NULL; int margin = -1;
  GOptionEntry entries[] = {
    { "write-defaults", 0, 0, G_OPTION_ARG_NONE, &write_defaults, "Write default ini and exit", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Print version", NULL },
    { "no-cache", 0, 0, G_OPTION_ARG_NONE, &no_cache, "Disable disk cache", NULL },
    { "font-family", 0, 0, G_OPTION_ARG_STRING, &font_family, "Font family", NULL },
    { "font-size", 0, 0, G_OPTION_ARG_INT, &font_size, "Font size", NULL },
    { "text-color", 0, 0, G_OPTION_ARG_STRING, &text_color, "Text color", NULL },
    { "bg-color", 0, 0, G_OPTION_ARG_STRING, &bg_color, "Background RRGGBBAA", NULL },
    { "width", 0, 0, G_OPTION_ARG_INT, &width, "Width", NULL },
    { "lines", 0, 0, G_OPTION_ARG_INT, &lines, "1 or 3", NULL },
    { "anchor", 0, 0, G_OPTION_ARG_STRING, &anchor, "top/bottom", NULL },
    { "margin", 0, 0, G_OPTION_ARG_INT, &margin, "Margin", NULL },
    { NULL }
  };
  GOptionContext *ctx = g_option_context_new("- synced lyrics overlay");
  g_option_context_add_main_entries(ctx, entries, NULL);
  GError *e = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &e)) {
    g_printerr("%s\n", e->message); return 1;
  }
  g_option_context_free(ctx);
  if (version) { g_print("lyrics-overlay-c 0.3.0\n"); return 0; }
  if (write_defaults) {
    Settings *d = settings_load_from(NULL, NULL);
    settings_save_user(d, user_ini_path());
    g_print("wrote %s\n", user_ini_path());
    return 0;
  }
  G.s = settings_load_defaults();
  if (font_family) { g_free(G.s->font_family); G.s->font_family = font_family; }
  if (font_size > 0) G.s->font_size = font_size;
  if (text_color) { g_free(G.s->text_color); G.s->text_color = text_color; }
  if (bg_color) { g_free(G.s->bg_color); G.s->bg_color = bg_color; }
  if (width > 0) G.s->width = width;
  if (lines == 1 || lines == 3) G.s->lines = lines;
  if (anchor) { g_free(G.s->anchor); G.s->anchor = anchor; }
  if (margin >= 0) G.s->margin = margin;
  if (no_cache) G.s->cache = FALSE;
  G.track_key = g_strdup(""); G.loading_key = g_strdup(""); G.shown_key = g_strdup("");
  G.shown_idx = -99;
  GStatBuf st;
  G.ini_mtime = (g_stat(user_ini_path(), &st) == 0) ? st.st_mtime : 0;
  G.gapp = gtk_application_new("io.github.lyrics-overlay",
                               G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(G.gapp, "activate", G_CALLBACK(on_activate), NULL);
  int rc = g_application_run(G_APPLICATION(G.gapp), 0, NULL);
  if (G.tracker) tracker_free(G.tracker);
  tracker_copy_free(&G.cur);
  lines_free(&G.lines);
  settings_free(G.s);
  g_free(G.track_key); g_free(G.loading_key); g_free(G.shown_key);
  return rc;
}
