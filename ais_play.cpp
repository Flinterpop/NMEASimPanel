/* ais_play.cpp -- see ais_play.h. */

#include "ais_play.h"

#include <assert.h>
#include <string.h>

/* A recording is bounded in practice (the bundled captures are ~2000 lines),
 * but every scan here still needs a hard ceiling so a corrupt length or a blob
 * with no terminator cannot spin forever. */
#define AIS_PLAY_MAX_LINES  100000u
#define AIS_PLAY_MAX_SCAN   (4u * 1024u * 1024u)

/* Cap a single inter-message wait. Real captures contain long idle stretches,
 * and a recording that pauses for ten minutes should not appear to have hung.
 * Mirrors the maxGap clamp in AIS_Streamer's replay. */
#define AIS_PLAY_MAX_GAP_S  60.0f

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

/* ---- minimal decode, for timing only -----------------------------------
 * The captures carry no per-line timestamps, so the only clock available is
 * the UTC inside base-station (type 4) and UTC-response (type 11) messages.
 * Recovering it needs just the first 78 bits of a payload, so this reads the
 * armor directly rather than pulling in a general AIS decoder -- ais_sim
 * stays encode-only. */

/* Inverse of ais_sim's six_to_armor. */
static int armor_to_six(char c) {
  int v = (int)(unsigned char)c - 48;
  if (v > 40) { v -= 8; }
  return v & 0x3F;
}

/* Read `n` bits (MSB-first, n <= 32) starting at bit `start` of an armored
 * payload, without dearmoring the whole thing. */
static uint32_t payload_bits(const char *payload, size_t plen, int start, int n) {
  assert(payload != NULL);
  assert(n > 0 && n <= 32);
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    const int    bit = start + i;
    const size_t idx = (size_t)(bit / 6);
    const uint32_t six = (idx < plen) ? (uint32_t)armor_to_six(payload[idx]) : 0u;
    v = (v << 1) | ((six >> (5 - (bit % 6))) & 1u);
  }
  return v;
}

/* Comma-separated field `idx` of a sentence, or NULL. */
static const char *field_at(const char *line, int idx, size_t *flen) {
  assert(line != NULL && flen != NULL);
  int f = 0;
  const char *start = line;
  for (int i = 0; i < AIS_LINE_MAX && line[i] != '\0'; i++) {
    if (line[i] == ',') {
      if (f == idx) { *flen = (size_t)(&line[i] - start); return start; }
      f++;
      start = &line[i + 1];
    }
  }
  if (f == idx) { *flen = strlen(start); return start; }
  return NULL;
}

/* Days since the Unix epoch for a proleptic-Gregorian date (Hinnant). */
static long days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const long     era = (long)((y >= 0 ? y : y - 399) / 400);
  const unsigned yoe = (unsigned)(y - (int)(era * 400));
  /* March-based month index, computed without negating an unsigned. */
  const unsigned mp  = (m > 2u) ? (m - 3u) : (m + 9u);
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + (long)doe - 719468L;
}

/* UTC from a single-fragment type 4 / 11 sentence. Returns 1 on success. */
static int utc_from_sentence(const char *line, uint32_t *epoch) {
  assert(line != NULL && epoch != NULL);

  size_t tl = 0, pl = 0;
  const char *tot = field_at(line, 1, &tl);   /* fragment count */
  const char *pay = field_at(line, 5, &pl);   /* armored payload */
  if (tot == NULL || pay == NULL) { return 0; }
  if (tl != 1u || tot[0] != '1') { return 0; }  /* multi-fragment: not a type 4 */
  if (pl < 13u) { return 0; }                   /* need 78 bits = 13 chars */

  const uint32_t type = payload_bits(pay, pl, 0, 6);
  if (type != 4u && type != 11u) { return 0; }

  /* 6 type + 2 repeat + 30 MMSI = 38, then year/month/day/hour/min/sec. */
  const uint32_t year = payload_bits(pay, pl, 38, 14);
  const uint32_t mon  = payload_bits(pay, pl, 52, 4);
  const uint32_t day  = payload_bits(pay, pl, 56, 5);
  const uint32_t hour = payload_bits(pay, pl, 61, 5);
  const uint32_t minu = payload_bits(pay, pl, 66, 6);
  const uint32_t sec  = payload_bits(pay, pl, 72, 6);

  /* Reject the "not available" sentinels and outright garbage. */
  if (year < 1970u || year > 2100u || mon < 1u || mon > 12u ||
      day  < 1u    || day  > 31u   || hour > 23u || minu > 59u || sec > 59u) {
    return 0;
  }

  *epoch = (uint32_t)(days_from_civil((int)year, mon, day) * 86400L
                      + (long)(hour * 3600u + minu * 60u + sec));
  return 1;
}

/* Longest non-decreasing run by epoch, order preserved. Real captures mix base
 * stations whose clocks disagree -- AIS_01.ais famously has 367 anchors saying
 * 2026 and 18 saying 2006 -- and those outliers would make replay jump
 * backwards. Keeping the longest consistent run discards them. O(n log n). */
static int longest_non_decreasing(AisAnchor *a, int n) {
  assert(a != NULL);
  if (n <= 1) { return n; }

  static int16_t prev[AIS_PLAY_MAX_ANCHORS];
  static int16_t tail_at[AIS_PLAY_MAX_ANCHORS];
  static uint32_t tail_epoch[AIS_PLAY_MAX_ANCHORS];
  int tails = 0;

  for (int i = 0; i < n; i++) {
    const uint32_t e = a[i].epoch;
    int lo = 0, hi = tails;                 /* upper_bound over tail_epoch */
    while (lo < hi) {
      const int mid = (lo + hi) / 2;
      if (tail_epoch[mid] <= e) { lo = mid + 1; } else { hi = mid; }
    }
    prev[i] = (lo > 0) ? tail_at[lo - 1] : (int16_t)-1;
    tail_epoch[lo] = e;
    tail_at[lo]    = (int16_t)i;
    if (lo == tails) { tails++; }
  }

  /* Walk the chain back, then reverse in place. */
  static AisAnchor out[AIS_PLAY_MAX_ANCHORS];
  int m = 0;
  for (int k = tail_at[tails - 1]; k >= 0 && m < n; k = prev[k]) { out[m++] = a[k]; }
  for (int i = 0; i < m; i++) { a[i] = out[m - 1 - i]; }
  return m;
}

/* Scan the log once, recovering a bounded set of timing anchors. Denser logs
 * are decimated rather than rejected: interpolation only needs enough anchors
 * to track drift. */
static int collect_anchors(AisPlay *p) {
  assert(p != NULL);

  int      count  = 0;
  int      stride = 1;
  int      seen   = 0;
  uint32_t idx    = 0;      /* message index, counting non-blank lines only */
  size_t   i      = 0;

  while (i < p->len && idx < AIS_PLAY_MAX_LINES) {
    char line[AIS_LINE_MAX];
    const size_t next = take_line(p->data, p->len, i, line, sizeof(line));
    if (line[0] != '\0') {
      uint32_t epoch = 0;
      if (utc_from_sentence(line, &epoch)) {
        if ((seen % stride) == 0) {
          if (count >= AIS_PLAY_MAX_ANCHORS) {
            /* Full: keep every other anchor and halve the sampling rate. */
            for (int k = 0; k < AIS_PLAY_MAX_ANCHORS / 2; k++) {
              p->anchors[k] = p->anchors[k * 2];
            }
            count   = AIS_PLAY_MAX_ANCHORS / 2;
            stride *= 2;
          }
          p->anchors[count].line  = idx;
          p->anchors[count].epoch = epoch;
          count++;
        }
        seen++;
      }
      idx++;
    }
    if (next <= i) { break; }
    i = next;
  }

  return longest_non_decreasing(p->anchors, count);
}

/* Virtual timestamp of message `idx`, interpolated by index between the
 * bracketing anchors. Outside the anchor span the value is clamped rather than
 * extrapolated -- extrapolating overshot the true duration by ~5x in
 * AIS_Streamer. anchor_at only ever moves forward, which is safe because
 * playback advances monotonically. */
static uint32_t ts_at(AisPlay *p, uint32_t idx) {
  assert(p != NULL);
  const int n = p->anchor_count;
  if (n < 2) { return 0; }

  const AisAnchor *a = p->anchors;
  if (idx <= a[0].line)     { return a[0].epoch; }
  if (idx >= a[n - 1].line) { return a[n - 1].epoch; }

  while (p->anchor_at + 1 < n && a[p->anchor_at + 1].line <= idx) { p->anchor_at++; }
  const int k = p->anchor_at;
  assert(k + 1 < n);

  const uint32_t i0 = a[k].line,  i1 = a[k + 1].line;
  const uint32_t e0 = a[k].epoch, e1 = a[k + 1].epoch;
  if (i1 == i0) { return e0; }

  const double frac = (double)(idx - i0) / (double)(i1 - i0);
  return (uint32_t)((double)e0 + frac * ((double)e1 - (double)e0));
}

uint32_t ais_play_span_s(const AisPlay *p) {
  assert(p != NULL);
  if (p == NULL || p->anchor_count < 2) { return 0u; }
  return p->anchors[p->anchor_count - 1].epoch - p->anchors[0].epoch;
}

int ais_play_init_original(AisPlay *p, const char *data, size_t len,
                           float delay_s, float speed, int loop) {
  assert(p != NULL && data != NULL);
  assert(speed > 0.0f);
  if (p == NULL || data == NULL || speed <= 0.0f) { return -1; }

  ais_play_init(p, data, len, delay_s, loop);
  p->speed        = speed;
  p->anchor_count = collect_anchors(p);
  p->anchor_at    = 0;
  /* Two anchors are the minimum that defines a rate, and they must actually
   * differ -- a zero span would make every gap zero and race through the log.
   * Below that the fixed delay set by ais_play_init stays in force. */
  p->original     = (p->anchor_count >= 2 && ais_play_span_s(p) > 0u) ? 1u : 0u;
  return p->anchor_count;
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
  p->cursor    = 0;
  p->line      = 0;
  p->due_s     = 0.0f;
  p->finished  = 0;
  p->anchor_at = 0;   /* ts_at() only walks forward, so it must reset too */
}

/* Wait before the message following the one just emitted. p->line has already
 * been incremented, so it is the 0-based index of that next message.
 *
 * A gap of zero is normal and correct -- a busy channel puts many messages in
 * the same second -- and cannot spin, because the emit loop is bounded by
 * max_lines. */
static float next_gap_s(AisPlay *p) {
  assert(p != NULL);
  if (!p->original) { return p->delay_s; }

  const uint32_t t0 = ts_at(p, (p->line > 0u) ? p->line - 1u : 0u);
  const uint32_t t1 = ts_at(p, p->line);
  float gap = (t1 > t0) ? (float)(t1 - t0) : 0.0f;
  gap /= (p->speed > 0.0f) ? p->speed : 1.0f;
  if (gap > AIS_PLAY_MAX_GAP_S) { gap = AIS_PLAY_MAX_GAP_S; }
  return gap;
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
      p->due_s += next_gap_s(p);
    }
    if (next <= prev) { p->finished = 1; break; }    /* no progress: bail out */
  }

  return n;
}
