/*
 * ais_play.h -- replay a recorded AIS log, line by line, on a paced schedule.
 *
 * Third sibling of nmea_sim / ais_sim: pure logic, no Arduino dependency, so
 * it unit tests on a PC. It does not parse or validate the sentences -- a
 * recording is replayed verbatim, which is what makes multi-fragment messages
 * (type 5 spans two lines) come out intact without the module understanding
 * them.
 *
 * The log is a NUL-terminated blob of newline-separated sentences, normally a
 * header generated from the captures in AIS_Recordings/ by
 * tools/make_ais_log.ps1. On the ESP32 a `const char[]` lives in flash and is
 * directly addressable, so no PROGMEM accessor is needed and the same pointer
 * works on the host.
 *
 * Pacing is cooperative, not threaded: the caller passes elapsed time and gets
 * back whatever is due. Nothing blocks, so the LVGL loop keeps running. This
 * is the one part that could NOT be carried over from AIS_Streamer's replay,
 * which owns a thread and sleeps.
 */

#ifndef AIS_PLAY_H
#define AIS_PLAY_H

#include <stdint.h>
#include <stddef.h>

#include "ais_sim.h"   /* AIS_LINE_MAX */

typedef struct {
  const char *data;         /* log blob; not owned, must outlive the struct */
  size_t      len;
  size_t      cursor;       /* byte offset of the next line to emit */
  uint32_t    line;         /* 1-based index of the last line emitted */
  uint32_t    lines_total;
  uint32_t    emitted;      /* running tally across loops */
  float       due_s;        /* seconds until the next line is released */
  float       delay_s;      /* fixed inter-sentence delay */
  uint8_t     loop;         /* restart at the end instead of stopping */
  uint8_t     finished;     /* set when a non-looping log runs out */
} AisPlay;

/* delay_s must be > 0. A log with no newline at all is treated as one line. */
void ais_play_init(AisPlay *p, const char *data, size_t len,
                   float delay_s, int loop);

/* Back to the first line; clears `finished`. Does not reset `emitted`. */
void ais_play_rewind(AisPlay *p);

/* Advance by dt_sec and write every line that has come due, up to max_lines.
 * Returns the count written, or -1 on bad args. Lines carry no CR/LF, matching
 * the nmea_sim / ais_sim contract. */
int ais_play_due(AisPlay *p, float dt_sec,
                 char lines[][AIS_LINE_MAX], int max_lines);

#endif /* AIS_PLAY_H */
