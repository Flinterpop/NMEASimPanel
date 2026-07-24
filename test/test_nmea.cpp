/* Host-side validation of nmea_sim.cpp -- no Arduino, no hardware. */
#include "nmea_sim.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_fail = 0;

/* Re-derive the checksum from the printed sentence and confirm the two hex
 * digits after '*' match. Proves both the XOR and the formatting. */
static void check_line(const char *tag, const char *line) {
    const char *star = strchr(line, '*');
    if (!star) { printf("  [FAIL] %s: no '*'\n", tag); g_fail++; return; }
    unsigned printed = 0;
    if (sscanf(star + 1, "%2x", &printed) != 1) {
        printf("  [FAIL] %s: no checksum hex\n", tag); g_fail++; return;
    }
    uint8_t calc = nmea_checksum(line);
    bool ok = ((uint8_t)printed == calc);
    printf("  [%s] %-4s %s   (cs %02X %s)\n", ok ? "PASS" : "FAIL", tag, line,
           calc, ok ? "ok" : "MISMATCH");
    if (!ok) g_fail++;
}

int main(void) {
    NmeaSim s;
    nmea_sim_defaults(&s);

    char buf[NMEA_LINE_MAX];
    printf("== defaults ==\n");
    nmea_build_rmc(&s, buf, sizeof(buf)); check_line("RMC", buf);
    nmea_build_gga(&s, buf, sizeof(buf)); check_line("GGA", buf);
    nmea_build_vtg(&s, buf, sizeof(buf)); check_line("VTG", buf);
    nmea_build_gll(&s, buf, sizeof(buf)); check_line("GLL", buf);

    printf("== registry ==\n");
    printf("  %d sentences:", NMEA_SENTENCE_COUNT);
    for (int i = 0; i < NMEA_SENTENCE_COUNT; i++) printf(" %s", NMEA_SENTENCES[i].name);
    printf("\n");

    printf("== enable mask 0b101 (RMC+VTG only) ==\n");
    char lines[8][NMEA_LINE_MAX];
    int n = nmea_sim_build_enabled(&s, 0x5u, lines, 8);
    printf("  built %d lines\n", n);
    for (int i = 0; i < n; i++) check_line("sel", lines[i]);
    if (n != 2) { printf("  [FAIL] expected 2 lines, got %d\n", n); g_fail++; }

    printf("== motion (10 ticks @1s, 150.1kn, hdg 45.1) ==\n");
    double lat0 = s.lat_deg, lon0 = s.lon_deg;
    for (int i = 0; i < 10; i++) nmea_sim_tick(&s, 1.0f);
    nmea_build_rmc(&s, buf, sizeof(buf)); check_line("RMC", buf);
    /* NE heading -> lat up, lon up (east). ~772 m over 10 s. */
    double dlat = s.lat_deg - lat0, dlon = s.lon_deg - lon0;
    printf("  dlat=%.6f dlon=%.6f (expect both > 0)\n", dlat, dlon);
    if (!(dlat > 0 && dlon > 0)) { printf("  [FAIL] motion direction\n"); g_fail++; }
    /* clock advanced 10 s */
    printf("  clock now %02u:%02u:%02u (expect 20:43:10)\n", s.hour, s.minute, s.second);
    if (!(s.hour == 20 && s.minute == 43 && s.second == 10)) { printf("  [FAIL] clock\n"); g_fail++; }

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
