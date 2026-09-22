#include "lyrics.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>

static const char * const SOURCES[] =
  { "local", "limusic", "mpris", "boidu", "lrclib",
    "netease", "qq", "kugou", NULL };
const char * const *lyric_sources(void) { return SOURCES; }

/* ---------- http ---------- */
static SoupSession *session(void) {
  static SoupSession *s = NULL;
  if (!s) { s = soup_session_new(); g_object_set(s, "timeout", 5, NULL); }
  return s;
}
static char *read_ok(SoupMessage *m, GBytes *b) {
  char *out = NULL;
  if (b && soup_message_get_status(m) == 200) {
    gsize n = 0; const char *d = g_bytes_get_data(b, &n);
    out = g_strndup(d, n);
  }
  return out;
}
/* one retry after 2s on rate-limit/server errors: at auto-change, two
   clients (us + the player) burst the same API in the same second. */
static int retryable(SoupMessage *m, GBytes *b) {
  int s = soup_message_get_status(m);
  return !b || s == 429 || s == 500 || s == 502 || s == 503;
}
static char *http_get(const char *url) {
  SoupMessage *m = soup_message_new("GET", url);
  if (!m) return NULL;
  soup_message_set_flags(m, SOUP_MESSAGE_NO_REDIRECT);
  GBytes *b = soup_session_send_and_read(session(), m, NULL, NULL);
  char *out = read_ok(m, b);
  if (!out && retryable(m, b)) {
    if (b) g_bytes_unref(b);
    g_usleep(2 * 1000 * 1000);
    b = soup_session_send_and_read(session(), m, NULL, NULL);
    out = read_ok(m, b);
  }
  if (b) g_bytes_unref(b);
  g_object_unref(m);
  return out;
}
static char *esc(const char *s) { return g_uri_escape_string(s ? s : "", NULL, TRUE); }
static char *send_retry(SoupMessage *m) {
  GBytes *b = soup_session_send_and_read(session(), m, NULL, NULL);
  char *out = read_ok(m, b);
  if (!out && retryable(m, b)) {
    if (b) g_bytes_unref(b);
    g_usleep(2 * 1000 * 1000);
    b = soup_session_send_and_read(session(), m, NULL, NULL);
    out = read_ok(m, b);
  }
  if (b) g_bytes_unref(b);
  g_object_unref(m);
  return out;
}
static char *http_post_form(const char *url, const char *form, const char *referer) {
  SoupMessage *m = soup_message_new("POST", url);
  if (!m) return NULL;
  soup_message_set_flags(m, SOUP_MESSAGE_NO_REDIRECT);
  if (referer) soup_message_headers_append(soup_message_get_request_headers(m),
                                           "Referer", referer);
  GBytes *body = g_bytes_new(form, strlen(form));
  soup_message_set_request_body_from_bytes(m, "application/x-www-form-urlencoded", body);
  g_bytes_unref(body);
  return send_retry(m);
}
static char *http_get_referer(const char *url, const char *referer) {
  SoupMessage *m = soup_message_new("GET", url);
  if (!m) return NULL;
  soup_message_set_flags(m, SOUP_MESSAGE_NO_REDIRECT);
  if (referer) soup_message_headers_append(soup_message_get_request_headers(m),
                                           "Referer", referer);
  return send_retry(m);
}

/* ---------- cache ---------- */
static void cache_key(const char *a, const char *t, double d, char out[64]) {
  char *sa = g_utf8_strdown(a ? a : "", -1), *st = g_utf8_strdown(t ? t : "", -1);
  char *dur = (d > 0) ? g_strdup_printf("%d", (int)d) : g_strdup("x");
  GChecksum *c = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(c, (guchar *)sa, -1); g_checksum_update(c, (guchar *)"", 1);
  g_checksum_update(c, (guchar *)st, -1); g_checksum_update(c, (guchar *)"", 1);
  g_checksum_update(c, (guchar *)dur, -1);
  g_strlcpy(out, g_checksum_get_string(c), 64);
  g_checksum_free(c); g_free(sa); g_free(st); g_free(dur);
}
static void cache_path(const char *a, const char *t, double d, char out[1100]) {
  char k[64]; cache_key(a, t, d, k);
  g_snprintf(out, 1100, "%s/%s.lrc", cache_dir(), k);
}
char *read_cache(const char *a, const char *t, double d) {
  char p[1100]; cache_path(a, t, d, p);
  char *txt = NULL;
  g_file_get_contents(p, &txt, NULL, NULL);
  return (txt && *txt) ? txt : (g_free(txt), NULL);
}
void write_cache(const char *a, const char *t, const char *text, double d) {
  if (!text || !*text) return;
  char p[1100]; cache_path(a, t, d, p);
  g_mkdir_with_parents(cache_dir(), 0755);
  g_file_set_contents(p, text, -1, NULL);
}

/* ---------- duration pick ---------- */
int best_dur_idx(int n, const double *durs, const int *has, double ours) {
  if (n <= 0) return -1;
  if (!(ours > 0)) return 0;
  int best = 0; double bd = 1e18;
  for (int i = 0; i < n; i++) {
    double dist = (has && !has[i]) ? 1e18 : fabs(durs[i] - ours);
    if (dist < bd) { bd = dist; best = i; }
  }
  if (bd != 1e18 && bd > 5.0) {
    for (int i = 0; i < n; i++)
      if (!has || !has[i]) return i;
    return -1;
  }
  return best;
}

/* ---------- lrc helpers ---------- */
int is_synced_lrc(const char *text) {
  if (!text) return 0;
  static GRegex *re = NULL;
  if (!re) re = g_regex_new("\\[[0-9]{1,3}:[0-9]{2}[.:][0-9]+\\]", 0, 0, NULL);
  return g_regex_match(re, text, 0, NULL);
}
static char *strip_tags(const char *s) {
  static GRegex *re = NULL;
  if (!re) re = g_regex_new("<[^>]+>", 0, 0, NULL);
  return g_regex_replace(re, s, -1, 0, "", 0, NULL);
}
char *ttml_to_lrc(const char *text) {
  if (!text || !strstr(text, "<p") || !strstr(text, "begin=")) return NULL;
  static GRegex *re = NULL;
  if (!re) re = g_regex_new("<p\\s+[^>]*begin=\"(?:([0-9]+):)?([0-9]+):([0-9.]+)\"[^>]*>(.*?)</p>",
                            G_REGEX_DOTALL | G_REGEX_CASELESS, 0, NULL);
  GMatchInfo *mi = NULL;
  g_regex_match(re, text, 0, &mi);
  GString *out = g_string_new(NULL);
  while (g_match_info_matches(mi)) {
    char *h = g_match_info_fetch(mi, 1), *mm = g_match_info_fetch(mi, 2);
    char *ss = g_match_info_fetch(mi, 3), *body = g_match_info_fetch(mi, 4);
    double total = atoi(h ? h : "0") * 3600 + atoi(mm) * 60 + g_ascii_strtod(ss, NULL);
    char *plain = strip_tags(body ? body : "");
    g_strstrip(plain);
    if (*plain) {
      for (char *c = plain; *c; c++) if (*c == '\n') *c = ' ';
      g_string_append_printf(out, "[%02d:%05.2f]%s\n",
                             (int)(total / 60), fmod(total, 60.0), plain);
    }
    g_free(h); g_free(mm); g_free(ss); g_free(body); g_free(plain);
    g_match_info_next(mi, NULL);
  }
  g_match_info_free(mi);
  if (!out->len) { g_string_free(out, TRUE); return NULL; }
  return g_string_free(out, FALSE);
}
static char *normalize_text(const char *text) {
  if (!text) return NULL;
  char *t = g_strstrip(g_strdup(text));
  if (!*t) { g_free(t); return NULL; }
  if (*t == '<' && (strstr(t, "<tt") || strstr(t, "<p"))) {
    char *c = ttml_to_lrc(t);
    g_free(t);
    if (!c || !*g_strstrip(c)) { g_free(c); return NULL; }
    return c;
  }
  return t;
}

/* ---------- local files ---------- */
static char *read_file(const char *p) {
  char *t = NULL;
  g_file_get_contents(p, &t, NULL, NULL);
  return (t && *t) ? t : (g_free(t), NULL);
}
char *find_local(const char *artist, const char *title, const char *url) {
  if (url && g_str_has_prefix(url, "file://")) {
    GFile *f = g_file_new_for_uri(url);
    char *path = g_file_get_path(f); g_object_unref(f);
    if (path) {
      char *dot = strrchr(path, '.');
      if (dot) {
        for (int e = 0; e < 2; e++) {
          char *cand = g_strdup_printf("%.*s%s", (int)(dot - path), path, e ? ".lrc" : ".lrcx");
          char *t = read_file(cand); g_free(cand);
          if (t) { g_free(path); return t; }
        }
      }
      g_free(path);
    }
  }
  const char *home = g_get_home_dir();
  char mus[1100]; const char *xdg = g_getenv("XDG_MUSIC_DIR");
  g_snprintf(mus, sizeof mus, "%s", xdg ? xdg : "");
  if (!*mus) g_snprintf(mus, sizeof mus, "%s/Music", home);
  char *names[2] = { NULL, NULL };
  if (artist && *artist && title && *title)
    names[0] = g_strdup_printf("%s - %s", artist, title);
  if (title && *title) names[1] = g_strdup(title);
  const char *dirs[4];
  char lyr[1100]; g_snprintf(lyr, sizeof lyr, "%s/.lyrics", home);
  dirs[0] = lyr; dirs[1] = mus; dirs[2] = home; dirs[3] = NULL;
  char *hit = NULL;
  for (int d = 0; d < 3 && !hit; d++)
    for (int i = 0; i < 2 && !hit; i++) {
      if (!names[i]) continue;
      for (int e = 0; e < 2 && !hit; e++) {
        char *p = g_strdup_printf("%s/%s%s", dirs[d], names[i], e ? ".lrc" : ".lrcx");
        hit = read_file(p); g_free(p);
      }
    }
  g_free(names[0]); g_free(names[1]);
  return hit;
}

/* ---------- limusic db ---------- */
static char *norm_sp(const char *s) {
  if (!s) return g_strdup("");
  char *l = g_utf8_strdown(s, -1);
  GRegex *re = g_regex_new("\\s+", 0, 0, NULL);
  char *r = g_regex_replace(re, l, -1, 0, " ", 0, NULL);
  g_regex_unref(re); g_free(l);
  return g_strstrip(r);
}
char *limusic_lyrics_for(const char *dbpath, const char *artist,
                         const char *title, double dur_s) {
  if ((!title || !*title) && (!artist || !*artist)) return NULL;
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    return NULL;
  char *nt = norm_sp(title), *na = norm_sp(artist);
  char *vids[64]; int nv = 0;
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(db, "SELECT video_id, song_json FROM plays ORDER BY played_at DESC",
                         -1, &q, NULL) == SQLITE_OK) {
    while (nv < 64 && sqlite3_step(q) == SQLITE_ROW) {
      const char *vid = (const char *)sqlite3_column_text(q, 0);
      const char *js = (const char *)sqlite3_column_text(q, 1);
      if (!vid || !js) continue;
      JsonParser *p = json_parser_new();
      if (!json_parser_load_from_data(p, js, -1, NULL)) { g_object_unref(p); continue; }
      JsonNode *root = json_parser_get_root(p);
      const char *t = "", *ar = "";
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *o = json_node_get_object(root);
        if (json_object_has_member(o, "title")) t = json_object_get_string_member(o, "title");
        if (json_object_has_member(o, "artists")) ar = json_object_get_string_member(o, "artists");
      }
      char *mt = norm_sp(t), *ma = norm_sp(ar);
      if (!strcmp(mt, nt) && *na && strstr(ma, na)) {
        int dup = 0;
        for (int i = 0; i < nv; i++) if (!strcmp(vids[i], vid)) dup = 1;
        if (!dup) vids[nv++] = g_strdup(vid);
      }
      g_free(mt); g_free(ma); g_object_unref(p);
    }
    sqlite3_finalize(q);
  }
  char *result = NULL;
  for (int i = 0; i < nv && !result; i++) {
    sqlite3_stmt *q2 = NULL;
    if (sqlite3_prepare_v2(db, "SELECT lyrics FROM lyrics_cache WHERE video_id=?",
                           -1, &q2, NULL) != SQLITE_OK) continue;
    sqlite3_bind_text(q2, 1, vids[i], -1, SQLITE_TRANSIENT);
    char *js = NULL;
    if (sqlite3_step(q2) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(q2, 0);
      if (v) js = g_strdup(v);
    }
    sqlite3_finalize(q2);
    if (!js) continue;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, js, -1, NULL)) {
      JsonNode *root = json_parser_get_root(p);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *o = json_node_get_object(root);
        gboolean synced = json_object_has_member(o, "synced") &&
                          json_object_get_boolean_member(o, "synced");
        gboolean instr = json_object_has_member(o, "instrumental") &&
                         json_object_get_boolean_member(o, "instrumental");
        if (synced && !instr && json_object_has_member(o, "lines")) {
          JsonArray *arr = json_object_get_array_member(o, "lines");
          int n = json_array_get_length(arr);
          double *ts = g_new(double, n); char **tx = g_new(char *, n);
          int m = 0; double last_end = 0;
          for (int k = 0; k < n; k++) {
            JsonObject *ln = json_array_get_object_element(arr, k);
            if (!ln || !json_object_has_member(ln, "time_ms")) continue;
            const char *tt = json_object_has_member(ln, "text") ?
                             json_object_get_string_member(ln, "text") : "";
            char *s = g_strstrip(g_strdup(tt ? tt : ""));
            if (!*s) { g_free(s); continue; }
            double tms = json_object_get_double_member(ln, "time_ms");
            double end = json_object_has_member(ln, "end_time_ms") ?
                         json_object_get_double_member(ln, "end_time_ms") : tms;
            if (end > last_end) last_end = end;
            ts[m] = tms; tx[m] = s; m++;
          }
          int ok = m >= 3;
          if (ok && dur_s > 0) {
            double le = last_end / 1000.0;
            if (le > dur_s + 12) ok = 0;
            double lo = dur_s * 0.5 < 45.0 ? dur_s * 0.5 : 45.0;
            if (le < lo) ok = 0;
          }
          if (ok) {
            for (int a = 0; a < m; a++)
              for (int b = a + 1; b < m; b++)
                if (ts[b] < ts[a]) {
                  double tt2 = ts[a]; ts[a] = ts[b]; ts[b] = tt2;
                  char *ss = tx[a]; tx[a] = tx[b]; tx[b] = ss;
                }
            GString *out = g_string_new(NULL);
            for (int a = 0; a < m; a++) {
              double t = ts[a] / 1000.0;
              g_string_append_printf(out, "[%02d:%05.2f]%s\n",
                                     (int)(t / 60), fmod(t, 60.0), tx[a]);
            }
            result = g_string_free(out, FALSE);
          }
          for (int a = 0; a < m; a++) g_free(tx[a]);
          g_free(ts); g_free(tx);
        }
      }
    }
    g_object_unref(p); g_free(js);
  }
  for (int i = 0; i < nv; i++) g_free(vids[i]);
  g_free(nt); g_free(na);
  sqlite3_close(db);
  return result;
}
static char *limusic_lyrics(const char *artist, const char *title, double dur_s) {
  char db[1100];
  g_snprintf(db, sizeof db, "%s/.local/share/com.limusic.desktop/limusic.sqlite",
             g_get_home_dir());
  if (!g_file_test(db, G_FILE_TEST_EXISTS)) return NULL;
  return limusic_lyrics_for(db, artist, title, dur_s);
}

/* ---------- boidu / lrclib ---------- */
static const char *jstr(JsonObject *o, const char *m) {
  if (!o || !json_object_has_member(o, m)) return NULL;
  JsonNode *n = json_object_get_member(o, m);
  if (n && JSON_NODE_HOLDS_VALUE(n) &&
      json_node_get_value_type(n) == G_TYPE_STRING)
    return json_node_get_string(n);
  return NULL;
}
/* numbers arrive as int or double depending on provider; read either */
static double jnum(JsonObject *o, const char *m, int *has) {
  if (has) *has = 0;
  if (!o || !json_object_has_member(o, m)) return 0;
  JsonNode *n = json_object_get_member(o, m);
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return 0;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_INT64) { if (has) *has = 1; return (double)json_node_get_int(n); }
  if (t == G_TYPE_DOUBLE) { if (has) *has = 1; return json_node_get_double(n); }
  return 0;
}
static char *fetch_boidu(const char *artist, const char *title,
                         const char *album, double dur_s) {
  if ((!artist || !*artist) && (!title || !*title)) return NULL;
  char *ea = esc(artist), *et = esc(title), *eal = esc(album);
  char *dur = (dur_s > 0) ? g_strdup_printf("&d=%d", (int)(dur_s + 0.5)) : g_strdup("");
  char *alb = (album && *album) ? g_strdup_printf("&al=%s", eal) : g_strdup("");
  char *url = g_strdup_printf("https://lyrics-api.boidu.dev/getLyrics?s=%s&a=%s%s%s",
                              et, ea, alb, dur);
  g_free(ea); g_free(et); g_free(eal); g_free(dur); g_free(alb);
  char *body = http_get(url); g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      const char *fields[] = { "ttml", "syncedLyrics", "lyrics", "lrc", NULL };
      for (int i = 0; fields[i] && !hit; i++) {
        const char *v = jstr(o, fields[i]);
        if (v && *v) {
          char *n = normalize_text(v);
          if (n && is_synced_lrc(n)) hit = n; else g_free(n);
        }
      }
    }
  }
  g_object_unref(p); g_free(body);
  return hit;
}
static char *fetch_lrclib(const char *artist, const char *title,
                          double dur_s, const char *album) {
  if (!artist || !*artist || !title || !*title) return NULL;
  char *ea = esc(artist), *et = esc(title), *eal = esc(album);
  char *dur = (dur_s > 0) ? g_strdup_printf("&duration=%d", (int)dur_s) : g_strdup("");
  char *alb = (album && *album) ? g_strdup_printf("&album_name=%s", eal) : g_strdup("");
  char *url = g_strdup_printf("https://lrclib.net/api/get?artist_name=%s&track_name=%s%s%s",
                              ea, et, alb, dur);
  g_free(ea); g_free(et); g_free(eal); g_free(dur); g_free(alb);
  char *body = http_get(url); g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      const char *v = jstr(json_node_get_object(root), "syncedLyrics");
      if (v && *v) hit = g_strdup(v);
    }
  }
  g_object_unref(p); g_free(body);
  return hit;
}
static char *fetch_lrclib_search(const char *artist, const char *title, double dur_s) {
  if (!artist || !*artist || !title || !*title) return NULL;
  char *ea = esc(artist), *et = esc(title);
  char *url = g_strdup_printf("https://lrclib.net/api/search?artist_name=%s&track_name=%s", ea, et);
  g_free(ea); g_free(et);
  char *body = http_get(url); g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
      JsonArray *arr = json_node_get_array(root);
      int n = json_array_get_length(arr);
      double *durs = g_new(double, n); int *has = g_new(int, n);
      const char **txt = g_new(const char *, n);
      int m = 0;
      for (int i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const char *sl = jstr(o, "syncedLyrics");
        if (!sl || !*sl) continue;
        { /* emptiness check on a copy: sl is borrowed parser memory */
          char *tmp = g_strstrip(g_strdup(sl));
          int empty = !*tmp;
          g_free(tmp);
          if (empty) continue;
        }
        txt[m] = sl;
        if (json_object_has_member(o, "duration")) {
          durs[m] = jnum(o, "duration", &has[m]);
        } else { durs[m] = 0; has[m] = 0; }
        m++;
      }
      int b = best_dur_idx(m, durs, has, dur_s);
      if (b >= 0) hit = g_strdup(txt[b]);
      g_free(durs); g_free(has); g_free(txt);
    }
  }
  g_object_unref(p); g_free(body);
  return hit;
}

/* ---------- negative cache: remember "no lyrics" for 24h ---------- */
#define MISS_TTL (24 * 3600)
static void miss_path(const char *a, const char *t, double d, char out[1100]) {
  char k[64]; cache_key(a, t, d, k);
  g_snprintf(out, 1100, "%s/%s.miss", cache_dir(), k);
}
int read_miss(const char *a, const char *t, double d) {
  char p[1100]; miss_path(a, t, d, p);
  GStatBuf st;
  if (g_stat(p, &st) != 0) return 0;
  time_t now = time(NULL);
  if ((double)(now - st.st_mtime) > MISS_TTL) { remove(p); return 0; }
  return 1;
}
void write_miss(const char *a, const char *t, double d) {
  char p[1100]; miss_path(a, t, d, p);
  g_mkdir_with_parents(cache_dir(), 0755);
  g_file_set_contents(p, "", 0, NULL);
}


/* ---------- fallback lane: only reached when faster sources miss ---------- */
static char *fetch_netease(const char *artist, const char *title, double dur_s) {
  if ((!artist || !*artist) && (!title || !*title)) return NULL;
  char *q = g_strdup_printf("%s %s", title ? title : "", artist ? artist : "");
  char *eq = esc(q); g_free(q);
  char *form = g_strdup_printf("s=%s&type=1&limit=5&offset=0", eq); g_free(eq);
  char *body = http_post_form("https://music.163.com/api/search/get", form,
                              "https://music.163.com/");
  g_free(form);
  if (!body) return NULL;
  long sid = 0;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      JsonObject *res = json_object_has_member(o, "result") ?
                        json_object_get_object_member(o, "result") : NULL;
      JsonArray *songs = (res && json_object_has_member(res, "songs")) ?
                         json_object_get_array_member(res, "songs") : NULL;
      if (songs) {
        int n = json_array_get_length(songs);
        double *durs = g_new(double, n); int *has = g_new(int, n);
        for (int i = 0; i < n; i++) {
          JsonObject *s = json_array_get_object_element(songs, i);
          durs[i] = jnum(s, "duration", &has[i]) / 1000.0;
        }
        int b = best_dur_idx(n, durs, has, dur_s);
        if (b >= 0) {
          JsonObject *s = json_array_get_object_element(songs, b);
          if (s && json_object_has_member(s, "id"))
            sid = (long)json_object_get_int_member(s, "id");
        }
        g_free(durs); g_free(has);
      }
    }
  }
  g_object_unref(p); g_free(body);
  if (!sid) return NULL;
  char *url = g_strdup_printf("https://music.163.com/api/song/lyric?id=%ld&lv=1&kv=1&tv=-1", sid);
  body = http_get_referer(url, "https://music.163.com/");
  g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      JsonObject *lrc = json_object_has_member(o, "lrc") ?
                        json_object_get_object_member(o, "lrc") : NULL;
      const char *t = lrc ? jstr(lrc, "lyric") : NULL;
      if (t && *t && is_synced_lrc(t)) hit = g_strdup(t);
    }
  }
  g_object_unref(p); g_free(body);
  return hit;
}
static char *maybe_b64(const char *s) {
  if (!s) return NULL;
  if ((int)strlen(s) > 200 && !strstr(s, "[")) {
    gsize n = 0; guchar *d = g_base64_decode(s, &n);
    if (d) { char *r = g_strndup((char *)d, n); g_free(d); return r; }
  }
  return g_strdup(s);
}
static char *fetch_qq(const char *artist, const char *title, double dur_s) {
  if ((!artist || !*artist) && (!title || !*title)) return NULL;
  char *q = g_strdup_printf("%s %s", title ? title : "", artist ? artist : "");
  char *eq = esc(q); g_free(q);
  char *url = g_strdup_printf("https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=%s&format=json", eq);
  g_free(eq);
  char *body = http_get_referer(url, "https://y.qq.com/");
  g_free(url);
  if (!body) return NULL;
  char *mid = NULL;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      JsonObject *data = json_object_has_member(o, "data") ?
                         json_object_get_object_member(o, "data") : NULL;
      JsonObject *song = (data && json_object_has_member(data, "song")) ?
                         json_object_get_object_member(data, "song") : NULL;
      JsonArray *list = (song && json_object_has_member(song, "list")) ?
                        json_object_get_array_member(song, "list") : NULL;
      if (list) {
        int n = json_array_get_length(list);
        double *durs = g_new(double, n); int *has = g_new(int, n);
        for (int i = 0; i < n; i++) {
          JsonObject *s = json_array_get_object_element(list, i);
          durs[i] = jnum(s, "interval", &has[i]);
        }
        int b = best_dur_idx(n, durs, has, dur_s);
        if (b >= 0) {
          JsonObject *s = json_array_get_object_element(list, b);
          const char *m = (s && json_object_has_member(s, "songmid")) ?
                          json_object_get_string_member(s, "songmid") : NULL;
          if (m) mid = g_strdup(m);
        }
        g_free(durs); g_free(has);
      }
    }
  }
  g_object_unref(p); g_free(body);
  if (!mid) return NULL;
  url = g_strdup_printf("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg"
                        "?songmid=%s&format=json&nobase64=1", mid);
  g_free(mid);
  body = http_get_referer(url, "https://y.qq.com/");
  g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      const char *raw = jstr(json_node_get_object(root), "lyric");
      if (raw && *raw) {
        char *t = maybe_b64(raw);
        if (t && is_synced_lrc(t)) hit = t; else g_free(t);
      }
    }
  }
  g_object_unref(p); g_free(body);
  return hit;
}
static char *fetch_kugou(const char *artist, const char *title, double dur_s) {
  if ((!artist || !*artist) && (!title || !*title)) return NULL;
  char *q = g_strdup_printf("%s %s", title ? title : "", artist ? artist : "");
  char *eq = esc(q); g_free(q);
  char *url = g_strdup_printf("https://songsearch.kugou.com/song_search_v2?keyword=%s&page=1&pagesize=5", eq);
  g_free(eq);
  char *body = http_get(url);
  g_free(url);
  if (!body) return NULL;
  char *hash = NULL;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      JsonObject *data = json_object_has_member(o, "data") ?
                         json_object_get_object_member(o, "data") : NULL;
      JsonArray *list = (data && json_object_has_member(data, "lists")) ?
                        json_object_get_array_member(data, "lists") : NULL;
      if (list) {
        int n = json_array_get_length(list);
        double *durs = g_new(double, n); int *has = g_new(int, n);
        for (int i = 0; i < n; i++) {
          JsonObject *s = json_array_get_object_element(list, i);
          durs[i] = jnum(s, "Duration", &has[i]);
        }
        int b = best_dur_idx(n, durs, has, dur_s);
        if (b >= 0) {
          JsonObject *s = json_array_get_object_element(list, b);
          const char *h = (s && json_object_has_member(s, "FileHash")) ?
                          json_object_get_string_member(s, "FileHash") : NULL;
          if (h) hash = g_strdup(h);
        }
        g_free(durs); g_free(has);
      }
    }
  }
  g_object_unref(p); g_free(body);
  if (!hash) return NULL;
  url = g_strdup_printf("https://krcs.kugou.com/search?ver=1&man=yes&client=mobi&hash=%s", hash);
  g_free(hash);
  body = http_get(url);
  g_free(url);
  if (!body) return NULL;
  char *id = NULL, *ak = NULL;
  p = json_parser_new();
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *o = json_node_get_object(root);
      JsonArray *c = json_object_has_member(o, "candidates") ?
                     json_object_get_array_member(o, "candidates") : NULL;
      if (c && json_array_get_length(c) > 0) {
        JsonObject *c0 = json_array_get_object_element(c, 0);
        const char *i = (c0 && json_object_has_member(c0, "id")) ?
                        json_object_get_string_member(c0, "id") : NULL;
        const char *a = (c0 && json_object_has_member(c0, "accesskey")) ?
                        json_object_get_string_member(c0, "accesskey") : NULL;
        if (i && a) { id = g_strdup(i); ak = g_strdup(a); }
      }
    }
  }
  g_object_unref(p); g_free(body);
  if (!id || !ak) { g_free(id); g_free(ak); return NULL; }
  url = g_strdup_printf("https://lyrics.kugou.com/download?ver=1&client=pc&id=%s&accesskey=%s&fmt=lrc",
                        id, ak);
  g_free(id); g_free(ak);
  body = http_get(url);
  g_free(url);
  if (!body) return NULL;
  char *hit = NULL;
  p = json_parser_new();
  gsize n = 0; guchar *dec = NULL;
  /* download body is {"content":"base64..."} */
  if (json_parser_load_from_data(p, body, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
      const char *c = jstr(json_node_get_object(root), "content");
      if (c && *c) dec = g_base64_decode(c, &n);
    }
  }
  g_object_unref(p); g_free(body);
  if (dec) {
    char *t = g_strndup((char *)dec, n);
    g_free(dec);
    if (t && is_synced_lrc(t)) hit = t; else g_free(t);
  }
  return hit;
}

/* ---------- chain ---------- */
char *run_source(const char *src, const char *artist, const char *title,
                 const char *url, double dur_s, const char *album,
                 const char *meta_lyrics) {
  if (!src) return NULL;
  if (!strcmp(src, "local")) return find_local(artist, title, url);
  if (!strcmp(src, "limusic")) return limusic_lyrics(artist, title, dur_s);
  if (!strcmp(src, "mpris")) {
    /* NOTE: never g_strstrip() the caller's string in place (it may be a
       literal or borrowed memory); normalize_text() copies first. */
    if (meta_lyrics && *meta_lyrics)
      return normalize_text(meta_lyrics);
    return NULL;
  }
  if (!strcmp(src, "boidu")) return fetch_boidu(artist, title, album, dur_s);
  if (!strcmp(src, "lrclib")) {
    char *h = fetch_lrclib(artist, title, dur_s, album);
    return h ? h : fetch_lrclib_search(artist, title, dur_s);
  }
  if (!strcmp(src, "netease")) return fetch_netease(artist, title, dur_s);
  if (!strcmp(src, "qq")) return fetch_qq(artist, title, dur_s);
  if (!strcmp(src, "kugou")) return fetch_kugou(artist, title, dur_s);
  return NULL;
}
char *fetch_first_synced(const char *artist, const char *title, const char *url,
                         double dur_s, const char *album, const char *meta_lyrics,
                         const char **src_name) {
  return fetch_first_synced_skip(artist, title, url, dur_s, album, meta_lyrics,
                                 src_name, NULL);
}
char *fetch_first_synced_skip(const char *artist, const char *title, const char *url,
                         double dur_s, const char *album, const char *meta_lyrics,
                         const char **src_name, const char *skip) {
  if (src_name) *src_name = NULL;
  for (int i = 0; SOURCES[i]; i++) {
    if (skip && !strcmp(SOURCES[i], skip)) continue;
    char *h = run_source(SOURCES[i], artist, title, url, dur_s, album, meta_lyrics);
    if (h && is_synced_lrc(h)) {
      if (src_name) *src_name = SOURCES[i];
      return h;
    }
    g_free(h);
  }
  return NULL;
}
