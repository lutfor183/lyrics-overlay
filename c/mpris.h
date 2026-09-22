#pragma once
#include <gio/gio.h>
typedef struct {
  char *bus, *status, *artist, *title, *album, *url, *meta_lyrics;
  gint64 dur_us, pos_us;
  double read_t;
} Player;
typedef struct Tracker Tracker;
Tracker *tracker_new(GDBusConnection *bus);
void tracker_free(Tracker *t);
void tracker_refresh(Tracker *t);        /* names + drop dead + read all meta */
void tracker_read_positions(Tracker *t); /* live Position for Playing */
void tracker_read_meta(Tracker *t);      /* live Status + Metadata for all */
int tracker_try_active(Tracker *t, Player *out); /* non-blocking UI copy; 0 = keep last */
void tracker_copy_free(Player *p);
void tracker_request_resync(Tracker *t);
void mpris_poke(void); /* implemented in main.c: schedule a UI check */
void tracker_set_resync_ms(Tracker *t, int ms);
char *track_key(const Player *p);        /* malloc'd "artist - title" */
