/* unit tests: lrc, duration pick, ttml, settings, limusic (fake db). no GUI. */
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include <unistd.h>
#include "common.h"
#include "lyrics.h"
static int fails = 0;
#define CHECK(name, cond) do { \
    printf("%s %s\n", (cond) ? "PASS" : "FAIL", name); \
    if (!(cond)) fails++; } while (0)
int main(void) {
  LyLines ls = parse_lrc("[ti:X]\n[00:12.00]First\n[00:17.50][00:18.00]Second\n\n[00:23.00]Third");
  CHECK("parse count", ls.n == 4);
  CHECK("parse order", ls.n == 4 && ls.v[0].t == 12.0 && ls.v[1].t == 17.5 &&
        ls.v[2].t == 18.0 && ls.v[3].t == 23.0);
  CHECK("parse text", ls.n > 0 && !strcmp(ls.v[0].text, "First"));
  CHECK("line_at before", line_at(&ls, 5.0) == -1);
  CHECK("line_at middle", line_at(&ls, 17.6) == 1);
  CHECK("line_at multi", line_at(&ls, 18.5) == 2);
  CHECK("line_at exact", line_at(&ls, 12.0) == 0);
  LyLines e2 = parse_lrc("");
  CHECK("parse empty", e2.n == 0);
  CHECK("line_at empty", line_at(&e2, 5.0) == -1);
  lines_free(&ls);
  double d[] = { 233.0, 263.0, 0.0 }; int h[] = { 1, 1, 0 };
  CHECK("dur closest", best_dur_idx(3, d, h, 235.0) == 0);
  CHECK("dur far->unknown", best_dur_idx(3, d, h, 100.0) == 2);
  CHECK("dur none->first", best_dur_idx(3, d, h, 0) == 0);
  CHECK("dur empty", best_dur_idx(0, d, h, 200.0) == -1);
  const char *ttml = "<tt><body><div><p begin=\"00:00:12.00\">Hello <span>world</span></p>"
                     "<p begin=\"00:01:05.50\">Second</p></div></body></tt>";
  char *c = ttml_to_lrc(ttml);
  CHECK("ttml converts", c && strstr(c, "[00:12.00]Hello world") && strstr(c, "[01:05.50]Second"));
  g_free(c);
  CHECK("ttml non-ttml", ttml_to_lrc("[00:12.00]plain") == NULL);
  CHECK("synced yes", is_synced_lrc("[00:12.00]hi"));
  CHECK("synced no", !is_synced_lrc("just words"));
  /* settings roundtrip via temp files */
  char tmp[256]; snprintf(tmp, sizeof tmp, "/tmp/lo-test-%d.ini", (int)getpid());
  Settings *s0 = settings_load_from(NULL, NULL);
  CHECK("defaults", s0->poll_ms == 100 && s0->sync_offset_ms == 350 &&
        !strcmp(s0->source, "auto") && s0->keep_above);
  g_free(s0->source); s0->source = g_strdup("lrclib");
  s0->keep_above = 0; s0->poll_ms = 100;
  settings_save_user(s0, tmp);
  Settings *s1 = settings_load_from(NULL, tmp);
  CHECK("settings persist", !strcmp(s1->source, "lrclib") && !s1->keep_above);
  settings_free(s0); settings_free(s1);
  remove(tmp);
  /* limusic against a fake db */
  char dbp[256]; snprintf(dbp, sizeof dbp, "/tmp/lo-fakedb-%d.sqlite", (int)getpid());
  remove(dbp);
  sqlite3 *db = NULL;
  CHECK("fake db open", sqlite3_open(dbp, &db) == SQLITE_OK);
  sqlite3_exec(db, "CREATE TABLE plays(video_id TEXT, song_json TEXT, played_at INT)", 0, 0, 0);
  sqlite3_exec(db, "CREATE TABLE lyrics_cache(video_id TEXT, lyrics TEXT, fetched_at INT)", 0, 0, 0);
  sqlite3_exec(db, "INSERT INTO plays VALUES('v1','{\"title\":\"T\",\"artists\":\"A\"}',9)", 0, 0, 0);
  sqlite3_exec(db, "INSERT INTO lyrics_cache VALUES('v1','{\"synced\":true,\"instrumental\":false,"
                   "\"lines\":[{\"time_ms\":1000,\"end_time_ms\":2000,\"text\":\"one\"},"
                   "{\"time_ms\":5000,\"end_time_ms\":6000,\"text\":\"two\"},"
                   "{\"time_ms\":9000,\"end_time_ms\":10000,\"text\":\"three\"}]}',9)", 0, 0, 0);
  sqlite3_close(db);
  char *r = limusic_lyrics_for(dbp, "A", "T", 15.0);
  CHECK("limusic hit", r && strstr(r, "[00:01.00]one") && strstr(r, "[00:09.00]three"));
  g_free(r);
  r = limusic_lyrics_for(dbp, "A", "Other", 15.0);
  CHECK("limusic title miss", r == NULL); g_free(r);
  r = limusic_lyrics_for(dbp, "A", "T", 1000.0);
  CHECK("limusic dur miss", r == NULL); g_free(r);
  r = limusic_lyrics_for("/nonexistent.sqlite", "A", "T", 15.0);
  CHECK("limusic nodb miss", r == NULL); g_free(r);
  remove(dbp);
  /* binary search over 2000 lines */
  GString *big = g_string_new(NULL);
  for (int i = 0; i < 2000; i++)
    g_string_append_printf(big, "[%02d:%05.2f]line%d\n", i / 60, (double)(i % 60), i);
  LyLines bl = parse_lrc(big->str);
  CHECK("big parse", bl.n == 2000);
  CHECK("bsearch mid", line_at(&bl, 999.5) == 999);
  CHECK("bsearch exact", line_at(&bl, 100.0) == 100);
  CHECK("bsearch before", line_at(&bl, 0.0) == 0);
  CHECK("bsearch neg", line_at(&bl, -1.0) == -1);
  CHECK("bsearch end", line_at(&bl, 5000.0) == 1999);
  lines_free(&bl); g_string_free(big, TRUE);
  /* negative cache */
  write_miss("A9", "T9", 11.0);
  CHECK("miss hit", read_miss("A9", "T9", 11.0) == 1);
  CHECK("miss other", read_miss("A9", "Other", 11.0) == 0);
  /* resync default */
  Settings *sr = settings_load_from(NULL, NULL);
  CHECK("resync default", sr->resync_ms == 5000);
  settings_free(sr);
  printf(fails ? "\n%d FAILURES\n" : "\nALL OK\n", fails);
  return fails != 0;
}
