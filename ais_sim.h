/*
 * ais_sim.h -- AIS (ITU-R M.1371) target simulator and !AIVDM encoder.
 *
 * Sibling of nmea_sim: pure logic, no display or Arduino I/O dependency, so it
 * can be unit tested on a PC. Where nmea_sim models OWN ship as a single state
 * struct, AIS reports OTHER vessels, so this module owns a small table of
 * targets and its own emission schedule.
 *
 * Ported from the WireCodecs `ais` codec (C:/source/WireCodecs/codec/ais),
 * which is the validated reference (unit tested, cross-checked against pyais).
 * The wire format is identical; the difference is that this port uses fixed
 * buffers throughout -- no std::vector/std::string, no allocation after init --
 * so it is safe on the ESP32 and satisfies the project's Power-of-10 rules.
 *
 * Bit order matters: AIS packs fields MSB-first (big-endian) and then armors
 * the stream in 6-bit groups. Do not "simplify" the writer to LSB-first.
 *
 * Builders produce bare sentences "!AIVDM,...*CS" with NO trailing CR/LF, so
 * the caller decides the line ending -- same contract as nmea_sim.
 */

#ifndef AIS_SIM_H
#define AIS_SIM_H

#include <stdint.h>
#include <stddef.h>

/* NMEA 0183 caps a sentence at 82 characters; keep the same headroom nmea_sim
 * uses so both modules can share a caller's line buffer. */
#define AIS_LINE_MAX      96

/* Type 5 is 424 bits -> 71 armored characters -> 2 fragments at 60 chars. */
#define AIS_MAX_BITS      424
#define AIS_BIT_BYTES     ((AIS_MAX_BITS + 7) / 8)
#define AIS_MAX_PAYLOAD   72          /* ceil(424/6) = 71, plus NUL */
#define AIS_MAX_FRAGMENTS 2

#define AIS_MAX_TARGETS   8
#define AIS_NAME_MAX      21          /* 20 six-bit chars + NUL */
#define AIS_CALLSIGN_MAX  8           /*  7 six-bit chars + NUL */

/* Fragment split threshold; keeps a sentence within the 82-char limit. */
#define AIS_MAX_PAYLOAD_CHARS 60

/* "Not available" sentinels, per ITU-R M.1371. */
#define AIS_HEADING_NA    511
#define AIS_ROT_NA        (-128)
#define AIS_TIMESTAMP_NA  60

typedef struct {
  uint32_t mmsi;
  char     name[AIS_NAME_MAX];
  char     call_sign[AIS_CALLSIGN_MAX];
  uint8_t  ship_type;        /* ITU-R M.1371 table; 70 = cargo, 30 = fishing */
  uint16_t dim_bow;          /* metres, 0..511 */
  uint16_t dim_stern;        /* metres, 0..511 */
  uint8_t  dim_port;         /* metres, 0..63  */
  uint8_t  dim_starboard;    /* metres, 0..63  */
  uint8_t  nav_status;       /* 0 = under way using engine, 15 = undefined */
  uint8_t  class_b;          /* 0 = Class A (types 1/5), 1 = Class B (18/24) */

  /* Live kinematics, advanced by ais_sim_tick(). */
  double   lat_deg;          /* +north, -south */
  double   lon_deg;          /* +east,  -west  */
  float    sog_kn;
  float    cog_deg;          /* 0..360 */
  float    turn_rate_dps;    /* degrees/sec added to cog each tick */
  int16_t  heading_deg;      /* 0..359, or AIS_HEADING_NA */

  /* Scheduler state: seconds until the next position / static report. */
  float    pos_due_s;
  float    static_due_s;
} AisTarget;

typedef struct {
  AisTarget targets[AIS_MAX_TARGETS];
  int       count;
  uint8_t   seq_id;          /* multi-fragment sequence id, cycles 0..9 */
  uint8_t   channel;         /* 'A' or 'B' */
  uint32_t  sentence_count;
} AisSim;

/* Seed a small, plausible traffic picture centred on (lat, lon). Targets are
 * placed a few miles off on differing courses so a plotter shows movement. */
void ais_sim_defaults(AisSim *s, double lat_deg, double lon_deg);

/* Advance every target by dt_sec of travel and count down its report timers.
 * dt_sec must be > 0. */
void ais_sim_tick(AisSim *s, float dt_sec);

/* Emit whatever is due this instant, resetting the timers that fired. Fills
 * lines[0..] and returns the count written (<= max_lines). Each buffer must be
 * at least AIS_LINE_MAX bytes. Call once per tick, after ais_sim_tick(). */
int ais_sim_build_due(AisSim *s, char lines[][AIS_LINE_MAX], int max_lines);

/* ---- Single-message builders --------------------------------------------
 * Each writes NUL-terminated sentences and returns the number of sentences
 * written, or -1 on bad args / overflow. Position reports are always one
 * sentence; type 5 is always two. */

/* Type 1 (Class A) or type 18 (Class B), chosen by t->class_b. */
int ais_build_position(const AisTarget *t, char *out, size_t cap);

/* Type 5 static & voyage data -- 424 bits, split across 2 fragments sharing
 * `seq_id`, with fill bits on the final fragment only. Class A. */
int ais_build_static_a(const AisTarget *t, uint8_t seq_id, uint8_t channel,
                       char lines[][AIS_LINE_MAX], int max_lines);

/* Type 24 static data report, Class B. part must be 0 (Part A, name) or
 * 1 (Part B, ship type / call sign / dimensions). */
int ais_build_static_b(const AisTarget *t, int part, uint8_t channel,
                       char *out, size_t cap);

/* XOR checksum of the characters between '!' and '*' (exclusive). */
uint8_t ais_checksum(const char *sentence);

#endif /* AIS_SIM_H */
