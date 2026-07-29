/*
 * ais_sim.cpp -- see ais_sim.h.
 *
 * Field order and scaling below are a direct transcription of the WireCodecs
 * `ais` codec encoders. If a field ever disagrees, that codec is the reference:
 * it has unit tests and has been cross-checked against the pyais decoder.
 */

#include "ais_sim.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- geometry, shared conventions with nmea_sim ------------------------- */

static const double kMetersPerKnotSec = 0.514444;
static const double kMetersPerDegLat  = 111320.0;
static const double kDeg2Rad          = 3.14159265358979323846 / 180.0;

/* ---- MSB-first bit writer ----------------------------------------------
 * Bits are packed into a fixed byte array, bit 0 = MSB of byte 0. AIS is
 * big-endian at the bit level; an LSB-first writer produces garbage that still
 * armors cleanly, so this is not a detail that fails loudly. */

typedef struct {
  uint8_t bytes[AIS_BIT_BYTES];
  int     nbits;
  int     overflow;
} AisBits;

static void bits_init(AisBits *b) {
  assert(b != NULL);
  memset(b->bytes, 0, sizeof(b->bytes));
  b->nbits    = 0;
  b->overflow = 0;
}

static void bits_put(AisBits *b, uint64_t value, int nbits) {
  assert(b != NULL);
  assert(nbits > 0 && nbits <= 64);
  if (b->nbits + nbits > AIS_MAX_BITS) { b->overflow = 1; return; }
  for (int i = nbits - 1; i >= 0; i--) {
    if ((value >> i) & 1u) {
      b->bytes[b->nbits >> 3] |= (uint8_t)(0x80u >> (b->nbits & 7));
    }
    b->nbits++;
  }
}

/* Two's-complement signed field: mask to width, then write as unsigned. */
static void bits_put_i(AisBits *b, int64_t value, int nbits) {
  assert(b != NULL);
  assert(nbits > 0 && nbits <= 64);
  const uint64_t mask = (nbits >= 64) ? ~0ull : ((1ull << nbits) - 1u);
  bits_put(b, (uint64_t)value & mask, nbits);
}

/* AIS 6-bit character set: '@'..'_' -> 0..31, ' '..'?' -> 32..63. */
static uint64_t char_to_six(char c) {
  unsigned uc = (unsigned char)c;
  if (uc >= 'a' && uc <= 'z') { uc = uc - 'a' + 'A'; }
  int v;
  if      (uc >= 64 && uc <= 95) { v = (int)uc - 64; }
  else if (uc >= 32 && uc <= 63) { v = (int)uc; }
  else                           { v = 0; }          /* unsupported -> '@' */
  return (uint64_t)(v & 0x3F);
}

/* Write `chars` six-bit characters, '@'-padded (AIS's NUL) or truncated. */
static void bits_put_text(AisBits *b, const char *s, int chars) {
  assert(b != NULL);
  assert(chars > 0 && chars <= 20);
  const size_t len = (s != NULL) ? strnlen(s, (size_t)chars) : 0u;
  for (int i = 0; i < chars; i++) {
    const char c = ((size_t)i < len) ? s[i] : '@';
    bits_put(b, char_to_six(c), 6);
  }
}

static char six_to_armor(int v) {
  assert(v >= 0 && v <= 63);
  v += 48;
  if (v > 87) { v += 8; }
  return (char)v;
}

/* Armor the bit stream into printable ASCII, padding to a 6-bit boundary.
 * Returns the character count, or -1 if `cap` is too small. */
static int bits_armor(const AisBits *b, char *out, size_t cap, int *fill_bits) {
  assert(b != NULL && out != NULL && fill_bits != NULL);
  assert(cap > 0);

  const int rem = b->nbits % 6;
  *fill_bits    = (rem == 0) ? 0 : (6 - rem);
  const int chars = (b->nbits + *fill_bits) / 6;
  if (chars < 0 || (size_t)chars + 1u > cap) { return -1; }

  for (int i = 0; i < chars && i < AIS_MAX_PAYLOAD; i++) {
    int v = 0;
    for (int j = 0; j < 6; j++) {
      const int idx = i * 6 + j;
      const int bit = (idx < b->nbits &&
                       (b->bytes[idx >> 3] & (0x80u >> (idx & 7)))) ? 1 : 0;
      v = (v << 1) | bit;
    }
    out[i] = six_to_armor(v);
  }
  out[chars] = '\0';
  return chars;
}

/* ---- NMEA framing ------------------------------------------------------- */

uint8_t ais_checksum(const char *sentence) {
  assert(sentence != NULL);
  assert(sentence[0] == '!');
  uint8_t chk = 0;
  const char *n = sentence + 1;
  for (int i = 0; i < AIS_LINE_MAX && *n != '\0' && *n != '*'; i++, n++) {
    chk ^= (uint8_t)*n;
  }
  return chk;
}

/* One "!AIVDM,total,index,seq,channel,payload,fill*CS". The sequence id field
 * is empty for single-fragment messages, which is what receivers expect. */
static int frame_one(char *out, size_t cap, int total, int index, int seq,
                     uint8_t channel, const char *payload, int fill) {
  assert(out != NULL && payload != NULL);
  assert(total >= 1 && index >= 1 && index <= total);
  assert(fill >= 0 && fill <= 5);

  const char ch = (channel == 'B') ? 'B' : 'A';
  int len;
  if (total > 1) {
    len = snprintf(out, cap, "!AIVDM,%d,%d,%d,%c,%s,%d",
                   total, index, seq % 10, ch, payload, fill);
  } else {
    len = snprintf(out, cap, "!AIVDM,%d,%d,,%c,%s,%d",
                   total, index, ch, payload, fill);
  }
  if (len < 0 || (size_t)len >= cap) { return -1; }

  const uint8_t cs = ais_checksum(out);
  const int w = snprintf(out + len, cap - (size_t)len, "*%02X", cs);
  if (w < 0 || (size_t)(len + w) >= cap) { return -1; }
  return len + w;
}

/* Armor + split + frame. Returns the sentence count, or -1 on overflow. */
static int emit(const AisBits *b, uint8_t seq, uint8_t channel,
                char lines[][AIS_LINE_MAX], int max_lines) {
  assert(b != NULL && lines != NULL);
  assert(max_lines >= 0);
  if (b->overflow) { return -1; }

  char payload[AIS_MAX_PAYLOAD];
  int  fill  = 0;
  const int chars = bits_armor(b, payload, sizeof(payload), &fill);
  if (chars <= 0) { return -1; }

  const int total = (chars + AIS_MAX_PAYLOAD_CHARS - 1) / AIS_MAX_PAYLOAD_CHARS;
  if (total > AIS_MAX_FRAGMENTS || total > max_lines) { return -1; }

  for (int i = 0; i < total && i < AIS_MAX_FRAGMENTS; i++) {
    char part[AIS_MAX_PAYLOAD_CHARS + 1];
    const int off = i * AIS_MAX_PAYLOAD_CHARS;
    int n = chars - off;
    if (n > AIS_MAX_PAYLOAD_CHARS) { n = AIS_MAX_PAYLOAD_CHARS; }
    assert(n > 0 && n <= AIS_MAX_PAYLOAD_CHARS);
    memcpy(part, payload + off, (size_t)n);
    part[n] = '\0';
    const int fb = (i == total - 1) ? fill : 0;
    if (frame_one(lines[i], AIS_LINE_MAX, total, i + 1, seq, channel, part, fb) < 0) {
      return -1;
    }
  }
  return total;
}

/* ---- field scaling (ITU-R M.1371) --------------------------------------- */

static long clamp_l(long v, long lo, long hi) {
  assert(lo <= hi);
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* Position is in 1/10000-minute units; 181 deg / 91 deg mean "not available". */
static long encode_lon(double deg) {
  if (deg >= 181.0 || deg < -180.0) { return 181L * 600000L; }
  return lround(deg * 600000.0);
}
static long encode_lat(double deg) {
  if (deg >= 91.0 || deg < -90.0) { return 91L * 600000L; }
  return lround(deg * 600000.0);
}
static int encode_sog(double knots) {            /* 0.1 kn units, 1023 = N/A */
  if (knots >= 102.3 || knots < 0.0) { return 1023; }
  return (int)clamp_l(lround(knots * 10.0), 0, 1022);
}
static int encode_cog(double deg) {              /* 0.1 deg units, 3600 = N/A */
  if (deg >= 360.0 || deg < 0.0) { return 3600; }
  return (int)clamp_l(lround(deg * 10.0), 0, 3599);
}

/* ---- message builders ---------------------------------------------------- */

/* Type 1 -- Position Report Class A. 168 bits, single fragment. */
static void pack_type1(AisBits *b, const AisTarget *t) {
  assert(b != NULL && t != NULL);
  bits_init(b);
  bits_put  (b, 1, 6);                              /* message type          */
  bits_put  (b, 0, 2);                              /* repeat indicator      */
  bits_put  (b, t->mmsi, 30);
  bits_put  (b, t->nav_status & 0xF, 4);
  bits_put_i(b, AIS_ROT_NA, 8);                     /* rate of turn: N/A     */
  bits_put  (b, (uint64_t)encode_sog(t->sog_kn), 10);
  bits_put  (b, 0, 1);                              /* position accuracy     */
  bits_put_i(b, encode_lon(t->lon_deg), 28);
  bits_put_i(b, encode_lat(t->lat_deg), 27);
  bits_put  (b, (uint64_t)encode_cog(t->cog_deg), 12);
  bits_put  (b, (uint64_t)(t->heading_deg & 0x1FF), 9);
  bits_put  (b, AIS_TIMESTAMP_NA & 0x3F, 6);
  bits_put  (b, 0, 2);                              /* manoeuvre indicator   */
  bits_put  (b, 0, 3);                              /* spare                 */
  bits_put  (b, 0, 1);                              /* RAIM                  */
  bits_put  (b, 0, 19);                             /* radio status          */
}

/* Type 18 -- Standard Class B CS Position Report. 168 bits, single fragment. */
static void pack_type18(AisBits *b, const AisTarget *t) {
  assert(b != NULL && t != NULL);
  bits_init(b);
  bits_put  (b, 18, 6);
  bits_put  (b, 0, 2);
  bits_put  (b, t->mmsi, 30);
  bits_put  (b, 0, 8);                              /* reserved              */
  bits_put  (b, (uint64_t)encode_sog(t->sog_kn), 10);
  bits_put  (b, 0, 1);
  bits_put_i(b, encode_lon(t->lon_deg), 28);
  bits_put_i(b, encode_lat(t->lat_deg), 27);
  bits_put  (b, (uint64_t)encode_cog(t->cog_deg), 12);
  bits_put  (b, (uint64_t)(t->heading_deg & 0x1FF), 9);
  bits_put  (b, AIS_TIMESTAMP_NA & 0x3F, 6);
  bits_put  (b, 0, 2);                              /* regional reserved     */
  bits_put  (b, 1, 1);                              /* CS unit               */
  bits_put  (b, 0, 1);                              /* display flag          */
  bits_put  (b, 0, 1);                              /* DSC flag              */
  bits_put  (b, 1, 1);                              /* band flag             */
  bits_put  (b, 1, 1);                              /* message 22 flag       */
  bits_put  (b, 0, 1);                              /* assigned              */
  bits_put  (b, 0, 1);                              /* RAIM                  */
  bits_put  (b, 0, 20);                             /* radio status          */
}

/* Type 5 -- Static and Voyage Related Data. 424 bits, two fragments. */
static void pack_type5(AisBits *b, const AisTarget *t) {
  assert(b != NULL && t != NULL);
  bits_init(b);
  bits_put     (b, 5, 6);
  bits_put     (b, 0, 2);
  bits_put     (b, t->mmsi, 30);
  bits_put     (b, 0, 2);                           /* AIS version           */
  bits_put     (b, 0, 30);                          /* IMO number            */
  bits_put_text(b, t->call_sign, 7);
  bits_put_text(b, t->name, 20);
  bits_put     (b, t->ship_type, 8);
  bits_put     (b, (uint64_t)clamp_l(t->dim_bow, 0, 511), 9);
  bits_put     (b, (uint64_t)clamp_l(t->dim_stern, 0, 511), 9);
  bits_put     (b, (uint64_t)clamp_l(t->dim_port, 0, 63), 6);
  bits_put     (b, (uint64_t)clamp_l(t->dim_starboard, 0, 63), 6);
  bits_put     (b, 1, 4);                           /* EPFD type: GPS        */
  bits_put     (b, 0, 4);                           /* ETA month  (N/A)      */
  bits_put     (b, 0, 5);                           /* ETA day    (N/A)      */
  bits_put     (b, 24, 5);                          /* ETA hour   (N/A)      */
  bits_put     (b, 60, 6);                          /* ETA minute (N/A)      */
  bits_put     (b, 0, 8);                           /* draught               */
  bits_put_text(b, "", 20);                         /* destination           */
  bits_put     (b, 0, 1);                           /* DTE                   */
  bits_put     (b, 0, 1);                           /* spare                 */
}

/* Type 24 Part A -- name only. 168 bits, single fragment. */
static void pack_type24a(AisBits *b, const AisTarget *t) {
  assert(b != NULL && t != NULL);
  bits_init(b);
  bits_put     (b, 24, 6);
  bits_put     (b, 0, 2);
  bits_put     (b, t->mmsi, 30);
  bits_put     (b, 0, 2);                           /* part number = A       */
  bits_put_text(b, t->name, 20);
  bits_put     (b, 0, 8);                           /* spare                 */
}

/* Type 24 Part B -- ship type, vendor, call sign, dimensions. 168 bits. */
static void pack_type24b(AisBits *b, const AisTarget *t) {
  assert(b != NULL && t != NULL);
  bits_init(b);
  bits_put     (b, 24, 6);
  bits_put     (b, 0, 2);
  bits_put     (b, t->mmsi, 30);
  bits_put     (b, 1, 2);                           /* part number = B       */
  bits_put     (b, t->ship_type, 8);
  bits_put_text(b, "SIM", 3);                       /* vendor id             */
  bits_put     (b, 0, 4);                           /* unit model code       */
  bits_put     (b, 0, 20);                          /* serial number         */
  bits_put_text(b, t->call_sign, 7);
  bits_put     (b, (uint64_t)clamp_l(t->dim_bow, 0, 511), 9);
  bits_put     (b, (uint64_t)clamp_l(t->dim_stern, 0, 511), 9);
  bits_put     (b, (uint64_t)clamp_l(t->dim_port, 0, 63), 6);
  bits_put     (b, (uint64_t)clamp_l(t->dim_starboard, 0, 63), 6);
  bits_put     (b, 0, 6);                           /* spare                 */
}

/* ---- public builders ----------------------------------------------------- */

int ais_build_position(const AisTarget *t, char *out, size_t cap) {
  assert(t != NULL);
  assert(out != NULL);
  if (t == NULL || out == NULL || cap < AIS_LINE_MAX) { return -1; }

  AisBits b;
  if (t->class_b) { pack_type18(&b, t); } else { pack_type1(&b, t); }

  char lines[1][AIS_LINE_MAX];
  const int n = emit(&b, 0, 'A', lines, 1);
  if (n != 1) { return -1; }
  memcpy(out, lines[0], strnlen(lines[0], AIS_LINE_MAX - 1) + 1u);
  return 1;
}

int ais_build_static_a(const AisTarget *t, uint8_t seq_id, uint8_t channel,
                       char lines[][AIS_LINE_MAX], int max_lines) {
  assert(t != NULL && lines != NULL);
  if (t == NULL || lines == NULL || max_lines < AIS_MAX_FRAGMENTS) { return -1; }

  AisBits b;
  pack_type5(&b, t);
  return emit(&b, seq_id, channel, lines, max_lines);
}

int ais_build_static_b(const AisTarget *t, int part, uint8_t channel,
                       char *out, size_t cap) {
  /* `part` is validated, not asserted: it is a documented -1 return, so a
   * defensive caller must be able to probe it without tripping a debug abort.
   * Asserts here are reserved for pointer contracts a caller cannot recover
   * from. */
  assert(t != NULL && out != NULL);
  if (t == NULL || out == NULL || cap < AIS_LINE_MAX) { return -1; }
  if (part != 0 && part != 1) { return -1; }

  AisBits b;
  if (part == 0) { pack_type24a(&b, t); } else { pack_type24b(&b, t); }

  char lines[1][AIS_LINE_MAX];
  const int n = emit(&b, 0, channel, lines, 1);
  if (n != 1) { return -1; }
  memcpy(out, lines[0], strnlen(lines[0], AIS_LINE_MAX - 1) + 1u);
  return 1;
}

/* ---- target simulation --------------------------------------------------- */

/* Reporting intervals, simplified from ITU-R M.1371's TDMA schedule: the real
 * rates depend on speed and manoeuvre state, but a plotter only cares that
 * position updates are frequent and static data is occasional. */
static float pos_interval_s(const AisTarget *t) {
  assert(t != NULL);
  if (t->class_b) { return (t->sog_kn > 2.0f) ? 30.0f : 180.0f; }
  return (t->sog_kn < 3.0f) ? 10.0f : 3.0f;
}
static float static_interval_s(void) { return 360.0f; }

void ais_sim_defaults(AisSim *s, double lat_deg, double lon_deg) {
  assert(s != NULL);
  assert(lat_deg >= -90.0 && lat_deg <= 90.0);

  memset(s, 0, sizeof(*s));
  s->channel = 'A';
  s->seq_id  = 0;

  /* A small, deliberately varied picture: two Class A ships on opposing
   * courses, a slow Class B, and a stationary Class B at anchor. Offsets are
   * a few minutes of arc so everything lands inside a typical plotter view. */
  static const struct {
    uint32_t mmsi;  const char *name; const char *call; uint8_t type;
    double   dlat;  double dlon;      float sog;        float cog;
    uint8_t  cls;   uint16_t bow;     uint16_t stern;   uint8_t port; uint8_t stbd;
  } kSeed[] = {
    { 316001234, "SIM CARGO ONE",  "CFA1234", 70, 0.030, -0.020, 12.5f,  95.0f, 0, 120, 30, 10, 12 },
    { 316005678, "SIM TANKER TWO", "CFB5678", 80, -0.025, 0.035,  9.0f, 271.0f, 0, 180, 40, 14, 16 },
    { 316009012, "SIM FISHER",     "CFC9012", 30, 0.012,  0.028,  4.2f,  20.0f, 1,  18,  4,  3,  3 },
    { 316003456, "SIM YACHT",      "CFD3456", 37, -0.018, -0.031, 0.0f,   0.0f, 1,  10,  3,  2,  2 },
  };
  const int seeds = (int)(sizeof(kSeed) / sizeof(kSeed[0]));

  for (int i = 0; i < seeds && i < AIS_MAX_TARGETS; i++) {
    AisTarget *t = &s->targets[i];
    t->mmsi = kSeed[i].mmsi;
    snprintf(t->name, sizeof(t->name), "%s", kSeed[i].name);
    snprintf(t->call_sign, sizeof(t->call_sign), "%s", kSeed[i].call);
    t->ship_type     = kSeed[i].type;
    t->lat_deg       = lat_deg + kSeed[i].dlat;
    t->lon_deg       = lon_deg + kSeed[i].dlon;
    t->sog_kn        = kSeed[i].sog;
    t->cog_deg       = kSeed[i].cog;
    t->heading_deg   = (int16_t)kSeed[i].cog;
    t->class_b       = kSeed[i].cls;
    t->dim_bow       = kSeed[i].bow;
    t->dim_stern     = kSeed[i].stern;
    t->dim_port      = kSeed[i].port;
    t->dim_starboard = kSeed[i].stbd;
    t->nav_status    = (kSeed[i].sog > 0.1f) ? 0u : 1u;   /* under way / anchored */
    t->turn_rate_dps = 0.0f;
    /* Stagger the first reports so they do not all land on the same tick. */
    t->pos_due_s     = (float)i * 0.5f;
    t->static_due_s  = (float)i * 2.0f;
    s->count++;
  }
}

void ais_sim_tick(AisSim *s, float dt_sec) {
  assert(s != NULL);
  assert(dt_sec > 0.0f);
  assert(s->count >= 0 && s->count <= AIS_MAX_TARGETS);

  for (int i = 0; i < s->count && i < AIS_MAX_TARGETS; i++) {
    AisTarget *t = &s->targets[i];

    const double dist_m  = (double)t->sog_kn * kMetersPerKnotSec * (double)dt_sec;
    const double cog_rad = (double)t->cog_deg * kDeg2Rad;
    const double cos_lat = cos(t->lat_deg * kDeg2Rad);

    /* Guard the pole singularity so the longitude step stays finite. */
    const double denom = kMetersPerDegLat * (fabs(cos_lat) > 1e-6 ? cos_lat : 1e-6);
    t->lat_deg += dist_m * cos(cog_rad) / kMetersPerDegLat;
    t->lon_deg += dist_m * sin(cog_rad) / denom;

    if (t->lat_deg >  90.0) { t->lat_deg =  90.0; }
    if (t->lat_deg < -90.0) { t->lat_deg = -90.0; }
    if (t->lon_deg > 180.0) { t->lon_deg -= 360.0; }
    if (t->lon_deg < -180.0) { t->lon_deg += 360.0; }

    t->cog_deg += t->turn_rate_dps * dt_sec;
    while (t->cog_deg >= 360.0f) { t->cog_deg -= 360.0f; }
    while (t->cog_deg <    0.0f) { t->cog_deg += 360.0f; }
    if (t->heading_deg != AIS_HEADING_NA) {
      t->heading_deg = (int16_t)t->cog_deg;
    }

    t->pos_due_s    -= dt_sec;
    t->static_due_s -= dt_sec;
  }
}

int ais_sim_build_due(AisSim *s, char lines[][AIS_LINE_MAX], int max_lines) {
  assert(s != NULL && lines != NULL);
  assert(max_lines >= 0);
  assert(s->count >= 0 && s->count <= AIS_MAX_TARGETS);

  int n = 0;
  for (int i = 0; i < s->count && i < AIS_MAX_TARGETS && n < max_lines; i++) {
    AisTarget *t = &s->targets[i];

    if (t->pos_due_s <= 0.0f) {
      if (ais_build_position(t, lines[n], AIS_LINE_MAX) == 1) { n++; }
      t->pos_due_s = pos_interval_s(t);
    }

    if (t->static_due_s <= 0.0f) {
      if (t->class_b) {
        /* Part A then Part B; emit only if both fit, so a plotter never sees a
         * half-identified target. */
        if (n + 2 <= max_lines) {
          if (ais_build_static_b(t, 0, s->channel, lines[n], AIS_LINE_MAX) == 1) { n++; }
          if (ais_build_static_b(t, 1, s->channel, lines[n], AIS_LINE_MAX) == 1) { n++; }
          t->static_due_s = static_interval_s();
        }
      } else {
        if (n + AIS_MAX_FRAGMENTS <= max_lines) {
          const int w = ais_build_static_a(t, s->seq_id, s->channel,
                                           &lines[n], max_lines - n);
          if (w > 0) { n += w; s->seq_id = (uint8_t)((s->seq_id + 1u) % 10u); }
          t->static_due_s = static_interval_s();
        }
      }
    }
  }

  s->sentence_count += (uint32_t)n;
  return n;
}
