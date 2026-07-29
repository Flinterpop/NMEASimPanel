/* Host-side validation of ais_play.cpp -- no Arduino, no hardware.
 *
 * Deliberately uses a small synthetic log rather than one of the real
 * recordings: this file is committed to a public repository, and the captures
 * are not. The playback engine never inspects sentence content, so synthetic
 * input exercises it exactly as well as a real capture would.
 */
#include "ais_play.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

static void check(const char *tag, bool ok, const char *detail) {
  printf("  [%s] %-12s %s\n", ok ? "PASS" : "FAIL", tag, detail ? detail : "");
  if (!ok) { g_fail++; }
}

static const char kLog[] =
  "!AIVDM,1,1,,A,LINE0001,0*00\n"
  "!AIVDM,1,1,,A,LINE0002,0*00\n"
  "!AIVDM,2,1,0,A,LINE0003a,0*00\n"
  "!AIVDM,2,2,0,A,LINE0003b,2*00\n"
  "!AIVDM,1,1,,A,LINE0005,0*00\n";

int main(void) {
  char lines[16][AIS_LINE_MAX];
  char msg[96];

  printf("== init ==\n");
  AisPlay p;
  ais_play_init(&p, kLog, sizeof(kLog) - 1u, 0.5f, 1);
  snprintf(msg, sizeof(msg), "%lu lines", (unsigned long)p.lines_total);
  check("count", p.lines_total == 5u, msg);

  printf("== pacing: 0.5 s delay, 1 s ticks -> 2 per tick ==\n");
  int n = ais_play_due(&p, 1.0f, lines, 16);
  snprintf(msg, sizeof(msg), "%d lines: %s | %s", n, n > 0 ? lines[0] : "-",
           n > 1 ? lines[1] : "-");
  check("tick1", n == 2, msg);
  check("order", n == 2 && strstr(lines[0], "LINE0001") != NULL &&
                 strstr(lines[1], "LINE0002") != NULL, "sequential");

  n = ais_play_due(&p, 1.0f, lines, 16);
  check("tick2", n == 2 && strstr(lines[0], "LINE0003a") != NULL,
        n == 2 ? lines[0] : "wrong count");
  /* Fragments must stay adjacent and in order or a receiver cannot reassemble. */
  check("fragments", n == 2 && strstr(lines[1], "LINE0003b") != NULL,
        "type 5 pair kept together");

  printf("== max_lines backpressure ==\n");
  AisPlay q;
  ais_play_init(&q, kLog, sizeof(kLog) - 1u, 0.1f, 1);
  n = ais_play_due(&q, 10.0f, lines, 3);      /* 100 due, room for 3 */
  snprintf(msg, sizeof(msg), "asked 3, got %d", n);
  check("clamped", n == 3, msg);

  printf("== looping ==\n");
  AisPlay r;
  ais_play_init(&r, kLog, sizeof(kLog) - 1u, 0.1f, 1);
  uint32_t seen = 0;
  for (int i = 0; i < 40; i++) { seen += (uint32_t)ais_play_due(&r, 1.0f, lines, 16); }
  snprintf(msg, sizeof(msg), "%lu emitted, finished=%u",
           (unsigned long)seen, (unsigned)r.finished);
  check("wraps", seen > 20u && r.finished == 0u, msg);

  printf("== non-looping stops ==\n");
  AisPlay s;
  ais_play_init(&s, kLog, sizeof(kLog) - 1u, 0.1f, 0);
  uint32_t total = 0;
  for (int i = 0; i < 20; i++) { total += (uint32_t)ais_play_due(&s, 1.0f, lines, 16); }
  snprintf(msg, sizeof(msg), "%lu emitted, finished=%u",
           (unsigned long)total, (unsigned)s.finished);
  check("stops", total == 5u && s.finished == 1u, msg);

  n = ais_play_due(&s, 1.0f, lines, 16);
  check("stays-done", n == 0, "no output after finish");

  ais_play_rewind(&s);
  n = ais_play_due(&s, 1.0f, lines, 16);
  check("rewind", n > 0 && strstr(lines[0], "LINE0001") != NULL,
        n > 0 ? lines[0] : "nothing");

  printf("== malformed input ==\n");
  /* CRLF, blank lines, no trailing newline, and a line past AIS_LINE_MAX. */
  static char big[AIS_LINE_MAX + 64];
  memset(big, 'X', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
  char blob[AIS_LINE_MAX + 128];
  snprintf(blob, sizeof(blob), "!A,ONE,0*00\r\n\r\n%s", big);
  AisPlay t;
  ais_play_init(&t, blob, strlen(blob), 0.1f, 0);
  snprintf(msg, sizeof(msg), "%lu (blank skipped)", (unsigned long)t.lines_total);
  check("lines", t.lines_total == 2u, msg);

  n = ais_play_due(&t, 1.0f, lines, 16);
  check("crlf", n >= 1 && strcmp(lines[0], "!A,ONE,0*00") == 0,
        n >= 1 ? lines[0] : "nothing");
  bool bounded = true;
  for (int i = 0; i < n; i++) { if (strlen(lines[i]) >= AIS_LINE_MAX) { bounded = false; } }
  check("truncated", bounded, "over-long line clamped, cursor intact");

  printf("== empty log ==\n");
  AisPlay u;
  ais_play_init(&u, "", 0u, 0.5f, 1);
  n = ais_play_due(&u, 1.0f, lines, 16);
  check("empty", n == 0, "no spin, no output");

  printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
