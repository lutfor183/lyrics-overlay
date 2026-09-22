#include "mpris.h"
#include "common.h"
#include <string.h>

struct Tracker {
  GDBusConnection *bus;
  GPtrArray *players; /* Player*, canonical state, lock-guarded */
  GMutex lock;
  GCond cond;
  GThread *thread;
  volatile gboolean run;
  volatile gboolean names_dirty, meta_dirty, resync_request;
  volatile int resync_ms;
  unsigned long nloops, last_pos_loop;
  guint name_sub, prop_sub;
};

static Player *find_player(Tracker *t, const char *bus) {
  for (guint i = 0; i < t->players->len; i++) {
    Player *p = t->players->pdata[i];
    if (!strcmp(p->bus, bus)) return p;
  }
  return NULL;
}
static void player_clear(Player *p) {
  if (!p) return;
  g_free(p->bus); g_free(p->status); g_free(p->artist); g_free(p->title);
  g_free(p->album); g_free(p->url); g_free(p->meta_lyrics);
  memset(p, 0, sizeof *p);
}
static void player_free(Player *p) {
  if (!p) return;
  player_clear(p);
  g_free(p);
}
static char *vstr(GVariant *v) {
  if (!v) return g_strdup("");
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
    return g_strdup(g_variant_get_string(v, NULL));
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
    const char **a = (const char **)g_variant_get_strv(v, NULL);
    char *r = g_strdup(a && a[0] ? a[0] : "");
    g_free(a); return r;
  }
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
    GVariant *inner = g_variant_get_variant(v);
    char *r = vstr(inner); g_variant_unref(inner); return r;
  }
  return g_strdup("");
}
/* sync Properties.Get; returns (v) unpacked or NULL */
static GVariant *prop_get(Tracker *t, const char *bus, const char *iface,
                          const char *prop) {
  GError *e = NULL;
  GVariant *res = g_dbus_connection_call_sync(
      t->bus, bus, "/org/mpris/MediaPlayer2",
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", iface, prop), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &e);
  if (e) { g_error_free(e); return NULL; }
  GVariant *v = NULL;
  g_variant_get(res, "(v)", &v);
  g_variant_unref(res);
  return v;
}
static void read_one_meta(Tracker *t, Player *p) {
  GVariant *v = prop_get(t, p->bus, "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
  if (v) { g_free(p->status); p->status = vstr(v); g_variant_unref(v); }
  v = prop_get(t, p->bus, "org.mpris.MediaPlayer2.Player", "Metadata");
  if (v) {
    GVariant *dict = v;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
      GVariant *inner = g_variant_get_variant(v);
      g_variant_unref(v); dict = inner;
    }
    if (dict && g_variant_is_of_type(dict, G_VARIANT_TYPE("a{sv}"))) {
      GVariant *e;
      e = g_variant_lookup_value(dict, "xesam:artist", NULL);
      if (e) { g_free(p->artist); p->artist = vstr(e); g_variant_unref(e); }
      e = g_variant_lookup_value(dict, "xesam:title", NULL);
      if (e) { g_free(p->title); p->title = vstr(e); g_variant_unref(e); }
      e = g_variant_lookup_value(dict, "xesam:album", NULL);
      if (e) { g_free(p->album); p->album = vstr(e); g_variant_unref(e); }
      e = g_variant_lookup_value(dict, "xesam:url", NULL);
      if (e) { g_free(p->url); p->url = vstr(e); g_variant_unref(e); }
      e = g_variant_lookup_value(dict, "mpris:length", NULL);
      if (e) {
        GVariant *u = e;
        if (g_variant_is_of_type(e, G_VARIANT_TYPE_VARIANT)) {
          u = g_variant_get_variant(e); g_variant_unref(e);
        }
        if (g_variant_is_of_type(u, G_VARIANT_TYPE_INT64))
          p->dur_us = g_variant_get_int64(u);
        else if (g_variant_is_of_type(u, G_VARIANT_TYPE_UINT64))
          p->dur_us = (gint64)g_variant_get_uint64(u);
        g_variant_unref(u);
      }
      const char *lk[] = { "xesam:lyrics", "mpris:lyrics", NULL };
      for (int i = 0; lk[i]; i++) {
        e = g_variant_lookup_value(dict, lk[i], NULL);
        if (e) {
          char *s = vstr(e); g_variant_unref(e);
          g_strstrip(s);
          if (*s) { g_free(p->meta_lyrics); p->meta_lyrics = s; }
          else g_free(s);
          break;
        }
      }
    }
    g_variant_unref(dict);
  }
}
static char **list_names(Tracker *t) {
  GError *e = NULL;
  GVariant *res = g_dbus_connection_call_sync(
      t->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &e);
  if (e) { g_error_free(e); return NULL; }
  /* NOTE: g_variant_get "(as)" extraction segfaulted here (glib quirk);
     iterate instead: s is borrowed, so strdup each name. */
  GVariant *arr = g_variant_get_child_value(res, 0);
  g_variant_unref(res);
  GPtrArray *out = g_ptr_array_new();
  GVariantIter it; const char *s = NULL;
  g_variant_iter_init(&it, arr);
  while (g_variant_iter_next(&it, "&s", &s))
    g_ptr_array_add(out, g_strdup(s));
  g_variant_unref(arr);
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}
static void on_name_owner(GDBusConnection *c, const char *sender, const char *path,
                          const char *iface, const char *sig, GVariant *params,
                          gpointer ud) {
  (void)c; (void)sender; (void)path; (void)iface; (void)sig; (void)params;
  Tracker *tt = (Tracker *)ud;
  g_mutex_lock(&tt->lock);
  tt->names_dirty = TRUE;
  g_cond_signal(&tt->cond);
  g_mutex_unlock(&tt->lock);
  mpris_poke();
}
static void on_props(GDBusConnection *c, const char *sender, const char *path,
                     const char *iface, const char *sig, GVariant *params,
                     gpointer ud) {
  (void)c; (void)path; (void)iface; (void)sig; (void)params;
  Tracker *t = ud;
  if (!sender || strncmp(sender, "org.mpris.MediaPlayer2.", 23)) return;
  g_mutex_lock(&t->lock);
  t->meta_dirty = TRUE;
  t->resync_request = TRUE; /* event: authoritative re-read immediately */
  g_cond_signal(&t->cond);
  g_mutex_unlock(&t->lock);
  mpris_poke();
}

static gpointer tracker_loop(gpointer ud) {
  Tracker *t = ud;
  gint64 last_names = 0, last_meta = 0, last_pos = 0;
  g_mutex_lock(&t->lock);
  while (t->run) {
    gint64 now = g_get_monotonic_time();
    int rms = t->resync_ms > 1000 ? t->resync_ms : 5000;
    gboolean names = t->names_dirty || now - last_names > 60 * G_USEC_PER_SEC;
    gboolean meta = t->meta_dirty || t->resync_request || now - last_meta > 5 * G_USEC_PER_SEC;
    gboolean pos = t->resync_request || now - last_pos > (gint64)rms * 1000;
    t->names_dirty = t->meta_dirty = t->resync_request = FALSE;
    if (!names && !meta && !pos) {
      /* nothing due: sleep until the next due time or a signal poke */
      gint64 wake = last_meta + 5 * G_USEC_PER_SEC;
      if (last_pos + (gint64)rms * 1000 < wake) wake = last_pos + (gint64)rms * 1000;
      if (last_names + 60 * G_USEC_PER_SEC < wake) wake = last_names + 60 * G_USEC_PER_SEC;
      if (wake < now + G_USEC_PER_SEC) wake = now + G_USEC_PER_SEC;
      g_cond_wait_until(&t->cond, &t->lock, wake);
      continue;
    }
    g_mutex_unlock(&t->lock);
    if (names) { tracker_refresh(t); last_names = g_get_monotonic_time(); }
    if (meta) { tracker_read_meta(t); last_meta = g_get_monotonic_time(); }
    if (pos) { tracker_read_positions(t); last_pos = g_get_monotonic_time(); }
    g_mutex_lock(&t->lock);
  }
  g_mutex_unlock(&t->lock);
  return NULL;
}
void tracker_request_resync(Tracker *t) {
  if (!t) return;
  g_mutex_lock(&t->lock);
  t->resync_request = TRUE;
  g_cond_signal(&t->cond);
  g_mutex_unlock(&t->lock);
}
void tracker_set_resync_ms(Tracker *t, int ms) {
  if (!t) return;
  g_mutex_lock(&t->lock);
  t->resync_ms = ms;
  g_cond_signal(&t->cond);
  g_mutex_unlock(&t->lock);
}
/* non-blocking copy of the active player for the UI tick; 0 = keep last */
int tracker_try_active(Tracker *t, Player *out) {
  if (!g_mutex_trylock(&t->lock)) return 0;
  const Player *a = NULL;
  for (guint i = 0; i < t->players->len && !a; i++) {
    const Player *p = t->players->pdata[i];
    if (!strcmp(p->status, "Playing") && (*p->artist || *p->title)) a = p;
  }
  for (guint i = 0; i < t->players->len && !a; i++) {
    const Player *p = t->players->pdata[i];
    if (!strcmp(p->status, "Paused") && (*p->artist || *p->title)) a = p;
  }
  int ok = 0;
  if (a) {
    out->bus = g_strdup(a->bus); out->status = g_strdup(a->status);
    out->artist = g_strdup(a->artist); out->title = g_strdup(a->title);
    out->album = g_strdup(a->album); out->url = g_strdup(a->url);
    out->meta_lyrics = g_strdup(a->meta_lyrics);
    out->dur_us = a->dur_us; out->pos_us = a->pos_us; out->read_t = a->read_t;
    ok = 1;
  }
  g_mutex_unlock(&t->lock);
  return ok;
}
/* UI copy with a small wait budget: a skipped copy leaves a stale anchor
   (late lyrics); a 50ms wait almost always succeeds. */
int tracker_wait_active(Tracker *t, Player *out) {
  for (int i = 0; i < 5; i++) {
    if (g_mutex_trylock(&t->lock)) {
      const Player *a = NULL;
      for (guint k = 0; k < t->players->len && !a; k++) {
        const Player *q = t->players->pdata[k];
        if (!strcmp(q->status, "Playing") && (*q->artist || *q->title)) a = q;
      }
      for (guint k = 0; k < t->players->len && !a; k++) {
        const Player *q = t->players->pdata[k];
        if (!strcmp(q->status, "Paused") && (*q->artist || *q->title)) a = q;
      }
      int ok = 0;
      if (a) {
        out->bus = g_strdup(a->bus); out->status = g_strdup(a->status);
        out->artist = g_strdup(a->artist); out->title = g_strdup(a->title);
        out->album = g_strdup(a->album); out->url = g_strdup(a->url);
        out->meta_lyrics = g_strdup(a->meta_lyrics);
        out->dur_us = a->dur_us; out->pos_us = a->pos_us; out->read_t = a->read_t;
        ok = 1;
      }
      g_mutex_unlock(&t->lock);
      return ok;
    }
    g_usleep(10 * 1000);
  }
  return tracker_try_active(t, out);
}
void tracker_copy_free(Player *p) {
  player_clear(p); /* embedded copy: free fields only, never the struct */
}

Tracker *tracker_new(GDBusConnection *bus) {
  Tracker *t = g_new0(Tracker, 1);
  g_mutex_init(&t->lock);
  g_cond_init(&t->cond);
  t->bus = g_object_ref(bus);
  t->players = g_ptr_array_new_with_free_func((GDestroyNotify)player_free);
  t->name_sub = g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner, t, NULL);
  t->prop_sub = g_dbus_connection_signal_subscribe(
      bus, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_props, t, NULL);
  tracker_refresh(t);
  t->run = TRUE;
  t->names_dirty = TRUE; t->meta_dirty = TRUE;
  t->thread = g_thread_new("mpris", tracker_loop, t);
  return t;
}
void tracker_free(Tracker *t) {
  if (!t) return;
  g_mutex_lock(&t->lock);
  t->run = FALSE;
  g_cond_signal(&t->cond);
  g_mutex_unlock(&t->lock);
  if (t->thread) g_thread_join(t->thread);
  g_cond_clear(&t->cond);
  g_dbus_connection_signal_unsubscribe(t->bus, t->name_sub);
  g_dbus_connection_signal_unsubscribe(t->bus, t->prop_sub);
  g_ptr_array_free(t->players, TRUE);
  g_object_unref(t->bus); g_free(t);
}
void tracker_refresh(Tracker *t) {
  g_mutex_lock(&t->lock);
  char **names = list_names(t);
  if (!names) { g_mutex_unlock(&t->lock); return; }
  GHashTable *live = g_hash_table_new(g_str_hash, g_str_equal);
  for (int i = 0; names[i]; i++)
    if (!strncmp(names[i], "org.mpris.MediaPlayer2.", 23))
      g_hash_table_add(live, names[i]);
  for (int i = (int)t->players->len - 1; i >= 0; i--) {
    Player *p = t->players->pdata[i];
    if (!g_hash_table_contains(live, p->bus))
      g_ptr_array_remove_index(t->players, i);
  }
  GHashTableIter it; gpointer k;
  g_hash_table_iter_init(&it, live);
  while (g_hash_table_iter_next(&it, &k, NULL)) {
    if (!find_player(t, k)) {
      Player *p = g_new0(Player, 1);
      p->bus = g_strdup(k);
      p->status = g_strdup("Stopped");
      p->artist = g_strdup(""); p->title = g_strdup("");
      p->album = g_strdup(""); p->url = g_strdup("");
      p->meta_lyrics = g_strdup("");
      g_ptr_array_add(t->players, p);
      read_one_meta(t, p);
    }
  }
  g_hash_table_unref(live);
  g_strfreev(names);
  g_mutex_unlock(&t->lock);
}

void tracker_read_positions(Tracker *t) {
  g_mutex_lock(&t->lock);
  for (guint i = 0; i < t->players->len; i++) {
    Player *p = t->players->pdata[i];
    if (strcmp(p->status, "Playing")) continue;
    GVariant *v = prop_get(t, p->bus, "org.mpris.MediaPlayer2.Player", "Position");
    if (v) {
      GVariant *u = v;
      if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
        u = g_variant_get_variant(v); g_variant_unref(v);
      }
      gint64 pos = p->pos_us;
      if (g_variant_is_of_type(u, G_VARIANT_TYPE_INT64))
        pos = g_variant_get_int64(u);
      else if (g_variant_is_of_type(u, G_VARIANT_TYPE_UINT64))
        pos = (gint64)g_variant_get_uint64(u);
      g_variant_unref(u);
      if (pos != p->pos_us) {
        /* value moved: this is a fresh sample, stamp it. Frozen repeats
           keep the old stamp so interpolation stays truthful. */
        p->pos_us = pos;
        p->read_t = g_get_monotonic_time() / 1e6;
      }
    }
  }
  g_mutex_unlock(&t->lock);
}
void tracker_read_meta(Tracker *t) {
  g_mutex_lock(&t->lock);
  for (guint i = 0; i < t->players->len; i++)
    read_one_meta(t, t->players->pdata[i]);
  g_mutex_unlock(&t->lock);
}
char *track_key(const Player *p) {
  if (!p) return g_strdup("");
  if (*p->artist && *p->title)
    return g_strdup_printf("%s - %s", p->artist, p->title);
  return g_strdup_printf("%s%s", p->artist, p->title);
}
