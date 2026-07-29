/* ais_play.cpp -- see ais_play.h. */

#include "ais_play.h"

#include <assert.h>
#include <string.h>

/* A recording is bounded in practice (the bundled captures are ~2000 lines),
 * but every scan here still needs a hard ceiling so a corrupt length or a blob
 * with no terminator cannot spin forever. */
#define AIS_PLAY_MAX_LINES  100000u
#define AIS_PLAY_MAX_SCAN   (4u * 1024u * 1024u)

/* Copy the line starting at `from` into `out`, returning the offset of the
 * next line. Trailing CR is dropped; an over-long line is truncated rather
 * than split, so a corrupt log cannot desynchronise the cursor. */
static size_t take_line(const char *data, size_t len, size_t from,
                        char *out, size_t cap) {
  assert(data != NULL && out != NULL);
  assert(cap > 0);

  size_t end = from;
  while (end < len && end - from < AIS_PLAY_MAX_SCAN && data[end] != '\n') { end++; }

  size_t n = end - from;
  while (n > 0 && data[from + n - 1] == '\r') { n--; }
  if (n > cap - 1u) { n = cap - 1u; }
  memcpy(out, data + from, n);
  out[n] = '\0';

  return (end < len) ? end + 1u : len;   /* step over the newline */
}

static uint32_t count_lines(const char *data, size_t len) {
  assert(data != NULL);
  uint32_t n = 0;
  size_t   i = 0;
  while (i < len && n < AIS_PLAY_MAX_LINES) {
    char scratch[AIS_LINE_MAX];
    const size_t next = take_line(data, len, i, scratch, sizeof(scratch));
    if (scratch[0] != '\0') { n++; }      /* ignore blank lines */
    if (next <= i) { break; }             /* no forward progress: stop */
    i = next;
  }
  return n;
}

void ais_play_init(AisPlay *p, const char *data, size_t len,
                   float delay_s, int loop) {
  assert(p != NULL);
  assert(data != NULL);
  assert(delay_s > 0.0f);

  memset(p, 0, sizeof(*p));
  p->data        = data;
  p->len         = len;
  p->delay_s     = (delay_s > 0.0f) ? delay_s : 1.0f;
  p->loop        = (loop != 0) ? 1u : 0u;
  p->lines_total = count_lines(data, len);
  p->due_s       = 0.0f;                  /* first line is due immediately */
}

void ais_play_rewind(AisPlay *p) {
  assert(p != NULL);
  p->cursor   = 0;
  p->line     = 0;
  p->due_s    = 0.0f;
  p->finished = 0;
}

int ais_play_due(AisPlay *p, float dt_sec,
                 char lines[][AIS_LINE_MAX], int max_lines) {
  assert(p != NULL && lines != NULL);
  assert(dt_sec > 0.0f);
  assert(max_lines >= 0);
  if (p == NULL || lines == NULL || max_lines < 0) { return -1; }
  if (p->data == NULL || p->finished) { return 0; }

  p->due_s -= dt_sec;

  int n = 0;
  /* Strictly < 0, not <= 0. With <=, a due_s that lands exactly on zero fires
   * an extra line, so the first tick emitted 3 at a 0.5 s delay where every
   * later tick emitted 2 -- and the equality made it sensitive to float
   * rounding. Strict comparison holds the true rate of dt/delay_s.
   *
   * Bounded by max_lines, so a tiny delay_s against a long dt cannot run away;
   * the backlog simply carries into the next call. */
  while (n < max_lines && p->due_s < 0.0f) {
    if (p->cursor >= p->len) {
      if (!p->loop) { p->finished = 1; break; }
      ais_play_rewind(p);
      if (p->len == 0) { p->finished = 1; break; }   /* empty log: don't spin */
    }

    const size_t next = take_line(p->data, p->len, p->cursor,
                                  lines[n], AIS_LINE_MAX);
    const size_t prev = p->cursor;
    p->cursor = next;

    if (lines[n][0] != '\0') {
      p->line++;
      p->emitted++;
      n++;
      p->due_s += p->delay_s;
    }
    if (next <= prev) { p->finished = 1; break; }    /* no progress: bail out */
  }

  return n;
}
