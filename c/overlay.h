#pragma once
#include <gtk/gtk.h>
#include "common.h"
typedef struct App App; /* main.c */
typedef struct Overlay Overlay;
Overlay *overlay_new(GtkApplication *app, Settings *s, App *cb);
GtkWindow *overlay_window(Overlay *o);
void overlay_refresh(Overlay *o, Settings *s);
void overlay_show_lyrics(Overlay *o, const char *prev, const char *cur, const char *next);
void overlay_show_single(Overlay *o, const char *cur);
void overlay_show_status(Overlay *o, const char *text);
void overlay_set_playing(Overlay *o, gboolean playing);
void overlay_open_settings(Overlay *o);
char *overlay_dump(Overlay *o);
/* callbacks implemented in main.c */
void app_refresh(App *a);
void app_quit(App *a);
void app_rearm(App *a);
