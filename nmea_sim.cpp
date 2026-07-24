#include "nmea_sim.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

static const double kDeg2Rad   = 3.14159265358979323846 / 180.0;
static const double kMetersPerDegLat = 111120.0;   /* 60 nm per degree */
static const double kMetersPerKnotSec = 1852.0 / 3600.0;

void nmea_sim_defaults(NmeaSim *s) {
  assert(s != NULL);
  s->lat_deg       = 45.255456;
  s->lon_deg       = -64.500000;   /* 64.5 W */
  s->alt_m         = 101.3f;
  s->speed_kn      = 150.1f;
  s->heading_deg   = 45.1f;

  s->turn_rate_dps = 0.0f;         /* straight line; set >0 for a circle */
  s->geoid_m       = 23.1f;
  s->mag_var_deg   = -3.1f;        /* 3.1 W */
  s->hdop          = 0.9f;
  s->fix_quality   = 1;
  s->num_sats      = 9;

  s->year   = 2026;
  s->month  = 7;
  s->day    = 23;
  s->hour   = 20;
  s->minute = 43;
  s->second = 0;

  s->sentence_count = 0;
}

/* Bounded, no library dependency: roll seconds up through the date. Only ever
 * advances, so a fixed cascade of if-statements covers any single tick. */
static void clock_add_seconds(NmeaSim *s, uint32_t secs) {
  assert(s != NULL);
  static const uint8_t kDaysInMonth[13] =
      {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  uint32_t total = (uint32_t)s->second + secs;
  s->second = (uint8_t)(total % 60u);
  uint32_t carry_min = total / 60u;
  if (carry_min == 0) { return; }

  total = (uint32_t)s->minute + carry_min;
  s->minute = (uint8_t)(total % 60u);
  uint32_t carry_hr = total / 60u;
  if (carry_hr == 0) { return; }

  total = (uint32_t)s->hour + carry_hr;
  s->hour = (uint8_t)(total % 24u);
  uint32_t carry_day = total / 24u;

  /* Bounded loop: at most a handful of day rollovers per realistic tick. */
  for (uint32_t i = 0; i < carry_day && i < 366u; i++) {
    uint8_t dim = kDaysInMonth[s->month];
    const bool leap = (s->year % 4 == 0 && (s->year % 100 != 0 || s->year % 400 == 0));
    if (s->month == 2 && leap) { dim = 29; }
    s->day++;
    if (s->day > dim) {
      s->day = 1;
      s->month++;
      if (s->month > 12) { s->month = 1; s->year++; }
    }
  }
}

void nmea_sim_tick(NmeaSim *s, float dt_sec) {
  assert(s != NULL);
  assert(dt_sec > 0.0f);

  const double dist_m   = (double)s->speed_kn * kMetersPerKnotSec * dt_sec;
  const double hdg_rad  = (double)s->heading_deg * kDeg2Rad;
  const double cos_lat  = cos(s->lat_deg * kDeg2Rad);

  const double dlat = dist_m * cos(hdg_rad) / kMetersPerDegLat;
  /* Guard the pole singularity so the longitude step stays finite. */
  const double denom = kMetersPerDegLat * (fabs(cos_lat) > 1e-6 ? cos_lat : 1e-6);
  const double dlon = dist_m * sin(hdg_rad) / denom;

  s->lat_deg += dlat;
  s->lon_deg += dlon;
  if (s->lat_deg >  90.0) { s->lat_deg =  90.0; }
  if (s->lat_deg < -90.0) { s->lat_deg = -90.0; }
  if (s->lon_deg > 180.0) { s->lon_deg -= 360.0; }
  if (s->lon_deg < -180.0) { s->lon_deg += 360.0; }

  s->heading_deg += s->turn_rate_dps * dt_sec;
  while (s->heading_deg >= 360.0f) { s->heading_deg -= 360.0f; }
  while (s->heading_deg <    0.0f) { s->heading_deg += 360.0f; }

  clock_add_seconds(s, (uint32_t)(dt_sec + 0.5f));
}

uint8_t nmea_checksum(const char *sentence) {
  assert(sentence != NULL);
  assert(sentence[0] == '$');
  uint8_t chk = 0;
  const char *n = sentence + 1;
  for (int i = 0; i < NMEA_LINE_MAX && *n != '\0' && *n != '*'; i++, n++) {
    chk ^= (uint8_t)*n;
  }
  return chk;
}

/* Append the "*CS" checksum to a body that currently ends at '*'... actually
 * the body is passed WITHOUT the star; we add "*CS". Returns total length or
 * -1 on overflow. */
static int finalize(char *out, size_t cap) {
  assert(out != NULL);
  const size_t len = strlen(out);
  if (len + 4 >= cap) { return -1; }          /* need '*','X','X','\0' */
  const uint8_t cs = nmea_checksum(out);
  const int w = snprintf(out + len, cap - len, "*%02X", cs);
  if (w < 0) { return -1; }
  return (int)(len + (size_t)w);
}

/* ddmm.mmmm into field, hemisphere into *hemi. */
static void fmt_lat(double lat, char *field, size_t cap, char *hemi) {
  assert(field != NULL && hemi != NULL);
  *hemi = (lat >= 0.0) ? 'N' : 'S';
  const double a   = fabs(lat);
  const int    deg = (int)a;
  const double min = (a - deg) * 60.0;
  snprintf(field, cap, "%02d%07.4f", deg, min);
}

static void fmt_lon(double lon, char *field, size_t cap, char *hemi) {
  assert(field != NULL && hemi != NULL);
  *hemi = (lon >= 0.0) ? 'E' : 'W';
  const double a   = fabs(lon);
  const int    deg = (int)a;
  const double min = (a - deg) * 60.0;
  snprintf(field, cap, "%03d%07.4f", deg, min);
}

int nmea_build_rmc(const NmeaSim *s, char *out, size_t cap) {
  assert(s != NULL && out != NULL);
  char lat[16], lon[16], nS, eW;
  fmt_lat(s->lat_deg, lat, sizeof(lat), &nS);
  fmt_lon(s->lon_deg, lon, sizeof(lon), &eW);

  const float  var  = s->mag_var_deg;
  const char   varh = (var >= 0.0f) ? 'E' : 'W';

  const int w = snprintf(
      out, cap,
      "$GPRMC,%02u%02u%02u,A,%s,%c,%s,%c,%.1f,%.1f,%02u%02u%02u,%05.1f,%c",
      s->hour, s->minute, s->second, lat, nS, lon, eW,
      (double)s->speed_kn, (double)s->heading_deg,
      s->day, s->month, (unsigned)(s->year % 100),
      (double)fabsf(var), varh);
  if (w < 0 || (size_t)w >= cap) { return -1; }
  return finalize(out, cap);
}

int nmea_build_gga(const NmeaSim *s, char *out, size_t cap) {
  assert(s != NULL && out != NULL);
  char lat[16], lon[16], nS, eW;
  fmt_lat(s->lat_deg, lat, sizeof(lat), &nS);
  fmt_lon(s->lon_deg, lon, sizeof(lon), &eW);

  const int w = snprintf(
      out, cap,
      "$GPGGA,%02u%02u%02u,%s,%c,%s,%c,%u,%02u,%.1f,%.1f,M,%.1f,M,,",
      s->hour, s->minute, s->second, lat, nS, lon, eW,
      s->fix_quality, s->num_sats, (double)s->hdop,
      (double)s->alt_m, (double)s->geoid_m);
  if (w < 0 || (size_t)w >= cap) { return -1; }
  return finalize(out, cap);
}

int nmea_build_vtg(const NmeaSim *s, char *out, size_t cap) {
  assert(s != NULL && out != NULL);
  /* Magnetic course = true course - variation (variation +east). */
  float mag = s->heading_deg - s->mag_var_deg;
  while (mag >= 360.0f) { mag -= 360.0f; }
  while (mag <    0.0f) { mag += 360.0f; }

  const int w = snprintf(
      out, cap,
      "$GPVTG,%.1f,T,%.1f,M,%.1f,N,%.1f,K,A",
      (double)s->heading_deg, (double)mag,
      (double)s->speed_kn, (double)s->speed_kn * 1.852);
  if (w < 0 || (size_t)w >= cap) { return -1; }
  return finalize(out, cap);
}

/* $GPGLL,ddmm.mmmm,N,dddmm.mmmm,W,hhmmss,A,A*CS
 * Trailing fields are status (A = valid) and the NMEA 2.3 mode indicator
 * (A = autonomous). */
int nmea_build_gll(const NmeaSim *s, char *out, size_t cap) {
  assert(s != NULL && out != NULL);
  char lat[16], lon[16], nS, eW;
  fmt_lat(s->lat_deg, lat, sizeof(lat), &nS);
  fmt_lon(s->lon_deg, lon, sizeof(lon), &eW);

  const int w = snprintf(out, cap, "$GPGLL,%s,%c,%s,%c,%02u%02u%02u,A,A",
                         lat, nS, lon, eW, s->hour, s->minute, s->second);
  if (w < 0 || (size_t)w >= cap) { return -1; }
  return finalize(out, cap);
}

/* The sentence registry. Add a row here (and its builder above) to introduce
 * a new sentence; the UI toggle and enable-mask bit follow automatically.
 * Append rather than insert so existing mask bit indices do not shift.
 * Keep the count <= 32. */
const NmeaSentenceDef NMEA_SENTENCES[] = {
  { "RMC", nmea_build_rmc },
  { "GGA", nmea_build_gga },
  { "VTG", nmea_build_vtg },
  { "GLL", nmea_build_gll },
  /* --- future GPS sentences (GSA, GSV, ...) and AIS go here --- */
};
const int NMEA_SENTENCE_COUNT =
    (int)(sizeof(NMEA_SENTENCES) / sizeof(NMEA_SENTENCES[0]));

int nmea_sim_build_enabled(const NmeaSim *s, uint32_t enable_mask,
                           char lines[][NMEA_LINE_MAX], int max_lines) {
  assert(s != NULL && lines != NULL);
  assert(max_lines >= 0);

  int n = 0;
  for (int i = 0; i < NMEA_SENTENCE_COUNT && n < max_lines; i++) {
    if ((enable_mask & (1u << i)) == 0) { continue; }
    if (NMEA_SENTENCES[i].build(s, lines[n], NMEA_LINE_MAX) > 0) { n++; }
  }
  return n;
}

int nmea_sim_build_all(const NmeaSim *s, char lines[][NMEA_LINE_MAX],
                       int max_lines) {
  return nmea_sim_build_enabled(s, 0xFFFFFFFFu, lines, max_lines);
}
