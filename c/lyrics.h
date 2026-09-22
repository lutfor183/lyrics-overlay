#pragma once
#include "common.h"
/* returns malloc'd LRC text or NULL; *src_name set to winner or NULL */
char *fetch_first_synced(const char *artist, const char *title, const char *url,
                         double dur_s, const char *album, const char *meta_lyrics,
                         const char **src_name);
char *run_source(const char *src, const char *artist, const char *title,
                 const char *url, double dur_s, const char *album,
                 const char *meta_lyrics);
char *read_cache(const char *artist, const char *title, double dur_s);
int read_miss(const char *artist, const char *title, double dur_s);
void write_miss(const char *artist, const char *title, double dur_s);
void  write_cache(const char *artist, const char *title, const char *text, double dur_s);
int   is_synced_lrc(const char *text);
/* testable helpers */
int   best_dur_idx(int n, const double *durs, const int *has, double ours);
char *ttml_to_lrc(const char *text);
char *limusic_lyrics_for(const char *dbpath, const char *artist,
                         const char *title, double dur_s);
const char * const *lyric_sources(void); /* NULL-terminated */
