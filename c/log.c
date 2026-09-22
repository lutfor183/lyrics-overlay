#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
static void ensure_dir(const char *p) { g_mkdir_with_parents(p, 0755); }
void app_log(const char *fmt, ...) {
  static char path[1024]; static int init = 0;
  if (!init) { g_snprintf(path, sizeof path, "%s/app.log", cache_dir()); init = 1; }
  ensure_dir(cache_dir());
  struct stat st;
  if (stat(path, &st) == 0 && st.st_size > 300 * 1024)
    rename(path, g_strconcat(path, ".old", NULL));
  time_t now = time(NULL); struct tm *tm = localtime(&now);
  FILE *f = fopen(path, "a");
  if (!f) return;
  fprintf(f, "%02d:%02d:%02d ", tm->tm_hour, tm->tm_min, tm->tm_sec);
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fputc('\n', f); fclose(f);
}
