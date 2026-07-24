/*
 * nmea_sim.h -- GPS NMEA-0183 sentence generator with dead-reckoned motion.
 *
 * Pure logic, no display or Arduino I/O dependencies, so it can be unit
 * tested on a PC and reused. First cut emits GPS (RMC/GGA/VTG); the
 * builder table (see nmea_sim_build_all) is the seam where an AIS source
 * will be added next.
 *
 * Builders produce a bare sentence "$....*CS" with NO trailing CR/LF, so
 * the caller decides the line ending (the wire wants "\r\n"; the on-screen
 * log wants "\n").
 */

#ifndef NMEA_SIM_H
#define NMEA_SIM_H

#include <stdint.h>
#include <stddef.h>

/* Longest sentence we generate is well under this. */
#define NMEA_LINE_MAX 96

typedef struct {
  /* Live position/attitude -- mutated by nmea_sim_tick(). */
  double lat_deg;        /* +north, -south            */
  double lon_deg;        /* +east,  -west             */
  float  alt_m;          /* MSL altitude, metres      */
  float  speed_kn;       /* speed over ground, knots  */
  float  heading_deg;    /* course over ground, 0..360 */

  /* Motion / fix parameters (not exposed in the first-cut UI). */
  float  turn_rate_dps;  /* degrees/sec added to heading each tick; 0 = straight */
  float  geoid_m;        /* geoidal separation for GGA */
  float  mag_var_deg;    /* magnetic variation, +east/-west */
  float  hdop;
  uint8_t fix_quality;   /* GGA fix indicator (0=none,1=GPS) */
  uint8_t num_sats;

  /* Synthetic UTC clock, advanced by nmea_sim_tick(). */
  uint16_t year;         /* full year, e.g. 2026 */
  uint8_t  month;        /* 1..12 */
  uint8_t  day;          /* 1..31 */
  uint8_t  hour;         /* 0..23 */
  uint8_t  minute;       /* 0..59 */
  uint8_t  second;       /* 0..59 */

  uint32_t sentence_count;
} NmeaSim;

/* Load sane defaults (the classic ad_GPS_SerialSim start point). */
void nmea_sim_defaults(NmeaSim *s);

/* Advance the clock by round(dt_sec) seconds and the position by dt_sec
 * worth of travel at the current speed/heading, then apply turn_rate.
 * dt_sec must be > 0. */
void nmea_sim_tick(NmeaSim *s, float dt_sec);

/* Sentence builders. Each writes a NUL-terminated "$....*CS" (no CR/LF) into
 * out and returns its length, or -1 on bad args / overflow. */
int nmea_build_rmc(const NmeaSim *s, char *out, size_t cap);
int nmea_build_gga(const NmeaSim *s, char *out, size_t cap);
int nmea_build_vtg(const NmeaSim *s, char *out, size_t cap);
int nmea_build_gll(const NmeaSim *s, char *out, size_t cap);

/* Sentence registry -- the single place to add a sentence type. Each row is
 * a display name plus its builder; the UI grows one enable/disable toggle
 * per row automatically, and enable-mask bit i corresponds to row i. */
typedef int (*nmea_builder_fn)(const NmeaSim *s, char *out, size_t cap);

typedef struct {
  const char     *name;   /* short label shown on the toggle, e.g. "RMC" */
  nmea_builder_fn build;
} NmeaSentenceDef;

extern const NmeaSentenceDef NMEA_SENTENCES[];
extern const int             NMEA_SENTENCE_COUNT;   /* <= 32 (mask is 32-bit) */

/* Build only the sentences whose registry bit is set in enable_mask, in table
 * order. Fills lines[0..] and returns the count written (<= max_lines). Each
 * buffer must be at least NMEA_LINE_MAX bytes. */
int nmea_sim_build_enabled(const NmeaSim *s, uint32_t enable_mask,
                           char lines[][NMEA_LINE_MAX], int max_lines);

/* Convenience: build every registered sentence (all bits set). */
int nmea_sim_build_all(const NmeaSim *s, char lines[][NMEA_LINE_MAX],
                       int max_lines);

/* XOR checksum of the chars between '$' and '*' (exclusive). */
uint8_t nmea_checksum(const char *sentence);

#endif /* NMEA_SIM_H */
