#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <regex.h>

static int cmp_line(const void *a, const void *b) {
  double d = ((const LyLine *)a)->t - ((const LyLine *)b)->t;
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}

LyLines parse_lrc(const char *text) {
  LyLines out = { NULL, 0 };
  if (!text) return out;
  regex_t re;
  regmatch_t m[4];
  if (regcomp(&re, "\\[([0-9]{1,3}):([0-9]{2})([.:]([0-9]{1,3}))?\\]", REG_EXTENDED))
    return out;
  char **rows = g_strsplit(text, "\n", -1);
  int cap = 0;
  for (int r = 0; rows[r]; r++) {
    /* collect all tags on this row */
    double times[16]; int nt = 0;
    const char *p = rows[r];
    while (nt < 16) {
      if (regexec(&re, p, 4, m, 0)) break;
      char mm[8] = {0}, ss[8] = {0}, fr[8] = {0};
      int l;
      l = m[1].rm_eo - m[1].rm_so; if (l > 7) l = 7;
      memcpy(mm, p + m[1].rm_so, l);
      l = m[2].rm_eo - m[2].rm_so; if (l > 7) l = 7;
      memcpy(ss, p + m[2].rm_so, l);
      if (m[3].rm_so >= 0) {
        int a = m[3].rm_so + 1, b = m[3].rm_eo, n = 0;
        char buf[4] = "000";
        for (int i = 0; i < 3 && a + i < b; i++) buf[i] = p[a + i];
        n = atoi(buf);
        g_snprintf(fr, sizeof fr, "%d", n);
      }
      times[nt++] = atoi(mm) * 60 + atoi(ss) + atoi(fr) / 1000.0;
      p += m[0].rm_eo;
    }
    if (!nt) continue;
    /* strip all tags for the lyric text */
    char *buf = g_strdup(rows[r]);
    char *dst = buf;
    p = rows[r];
    while (*p) {
      if (*p == '[' && regexec(&re, p, 4, m, 0) == 0 && m[0].rm_so == 0) {
        p += m[0].rm_eo;
      } else {
        *dst++ = *p++;
      }
    }
    *dst = 0;
    g_strstrip(buf);
    if (!*buf) { g_free(buf); continue; }
    for (int i = 0; i < nt; i++) {
      if (out.n == cap) { cap = cap ? cap * 2 : 16; out.v = g_realloc(out.v, cap * sizeof *out.v); }
      out.v[out.n].t = times[i];
      out.v[out.n].text = g_strdup(buf);
      out.n++;
    }
    g_free(buf);
  }
  g_strfreev(rows);
  regfree(&re);
  qsort(out.v, out.n, sizeof *out.v, cmp_line);
  return out;
}

void lines_free(LyLines *ls) {
  if (!ls) return;
  for (int i = 0; i < ls->n; i++) g_free(ls->v[i].text);
  g_free(ls->v); ls->v = NULL; ls->n = 0;
}

int line_at(const LyLines *ls, double pos) {
  /* binary search: last line with t <= pos */
  if (!ls || !ls->n) return -1;
  int lo = -1, hi = ls->n;
  while (hi - lo > 1) {
    int mid = lo + (hi - lo) / 2;
    if (ls->v[mid].t <= pos) lo = mid; else hi = mid;
  }
  return lo;
}
