/* Host-side validation of ais_sim.cpp -- no Arduino, no hardware.
 *
 * The golden sentences below were produced by the WireCodecs `ais` codec
 * (C:/source/WireCodecs/codec/ais), which is unit tested and cross-checked
 * against the independent pyais decoder. They are embedded rather than
 * generated so this test has no dependency on that repo.
 *
 * If a golden vector ever fails, suspect this port -- not the vector. Verify
 * against WireCodecs before changing anything here.
 */
#include "ais_sim.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int g_fail = 0;

static void check(const char *tag, bool ok, const char *detail) {
  printf("  [%s] %-10s %s\n", ok ? "PASS" : "FAIL", tag, detail ? detail : "");
  if (!ok) { g_fail++; }
}

static void check_golden(const char *tag, const char *got, const char *want) {
  const bool ok = (strcmp(got, want) == 0);
  printf("  [%s] %-10s %s\n", ok ? "PASS" : "FAIL", tag, got);
  if (!ok) { printf("            expected %s\n", want); g_fail++; }
}

/* Re-derive the checksum from the printed sentence; proves XOR + formatting. */
static void check_checksum(const char *tag, const char *line) {
  const char *star = strchr(line, '*');
  if (!star) { check(tag, false, "no '*'"); return; }
  unsigned printed = 0;
  if (sscanf(star + 1, "%2x", &printed) != 1) { check(tag, false, "no cs hex"); return; }
  check(tag, (uint8_t)printed == ais_checksum(line), "checksum");
}

/* NMEA 0183 caps a sentence at 82 chars including the CRLF we append later. */
static void check_length(const char *tag, const char *line) {
  const size_t n = strlen(line);
  char msg[64];
  snprintf(msg, sizeof(msg), "%u chars (limit 80 + CRLF)", (unsigned)n);
  check(tag, n <= 80u, msg);
}

static AisTarget target_a(void) {
  AisTarget t;
  memset(&t, 0, sizeof(t));
  t.mmsi = 316001234u;
  snprintf(t.name, sizeof(t.name), "%s", "SIM CARGO ONE");
  snprintf(t.call_sign, sizeof(t.call_sign), "%s", "CFA1234");
  t.ship_type = 70;
  t.lat_deg = 45.285456; t.lon_deg = -64.520000;
  t.sog_kn = 12.5f; t.cog_deg = 95.0f; t.heading_deg = 95;
  t.dim_bow = 120; t.dim_stern = 30; t.dim_port = 10; t.dim_starboard = 12;
  t.nav_status = 0; t.class_b = 0;
  return t;
}

static AisTarget target_b(void) {
  AisTarget t;
  memset(&t, 0, sizeof(t));
  t.mmsi = 316009012u;
  snprintf(t.name, sizeof(t.name), "%s", "SIM FISHER");
  snprintf(t.call_sign, sizeof(t.call_sign), "%s", "CFC9012");
  t.ship_type = 30;
  t.lat_deg = 45.267456; t.lon_deg = -64.472000;
  t.sog_kn = 4.2f; t.cog_deg = 20.0f; t.heading_deg = 20;
  t.dim_bow = 18; t.dim_stern = 4; t.dim_port = 3; t.dim_starboard = 3;
  t.nav_status = 0; t.class_b = 1;
  return t;
}

int main(void) {
  const AisTarget ta = target_a();
  const AisTarget tb = target_b();

  char one[AIS_LINE_MAX];
  char frag[AIS_MAX_FRAGMENTS][AIS_LINE_MAX];

  printf("== golden vectors (vs WireCodecs reference) ==\n");

  if (ais_build_position(&ta, one, sizeof(one)) != 1) { check("type1", false, "build"); }
  else {
    check_golden("type1", one, "!AIVDM,1,1,,A,14eG;lPP1uKHab0IrIjSeRwp0000,0*59");
    check_checksum("type1.cs", one);
    check_length("type1.len", one);
  }

  if (ais_build_position(&tb, one, sizeof(one)) != 1) { check("type18", false, "build"); }
  else {
    check_golden("type18", one, "!AIVDM,1,1,,A,B4eGb=00:Vn=sP6NKq`<P:N4h000,0*67");
    check_checksum("type18.cs", one);
    check_length("type18.len", one);
  }

  int n = ais_build_static_a(&ta, 0, 'A', frag, AIS_MAX_FRAGMENTS);
  if (n != 2) { check("type5", false, "expected 2 fragments"); }
  else {
    check_golden("type5.1", frag[0],
        "!AIVDM,2,1,0,A,54eG;lP00000<H77;?A<Tn0<58Lv0tpD00000016?0N:<40Ht00000000000,0*35");
    check_golden("type5.2", frag[1], "!AIVDM,2,2,0,A,00000000000,2*24");
    check_checksum("type5.1.cs", frag[0]);
    check_checksum("type5.2.cs", frag[1]);
    check_length("type5.1.len", frag[0]);
    check_length("type5.2.len", frag[1]);
    /* Fill bits belong on the FINAL fragment only -- a classic AIS bug. */
    check("type5.fill", strstr(frag[0], ",0*") != NULL && strstr(frag[1], ",2*") != NULL,
          "fill 0 then 2");
  }

  if (ais_build_static_b(&tb, 0, 'A', one, sizeof(one)) != 1) { check("type24a", false, "build"); }
  else {
    check_golden("type24a", one, "!AIVDM,1,1,,A,H4eGb=1<Tn0HU<PE800000000000,0*1C");
    check_checksum("type24a.cs", one);
  }
  if (ais_build_static_b(&tb, 1, 'A', one, sizeof(one)) != 1) { check("type24b", false, "build"); }
  else {
    check_golden("type24b", one, "!AIVDM,1,1,,A,H4eGb=4NC9=0000363qhij2@4330,0*40");
    check_checksum("type24b.cs", one);
  }

  printf("== argument validation ==\n");
  check("cap.small", ais_build_position(&ta, one, 8u) == -1, "tiny cap rejected");
  check("part.bad",  ais_build_static_b(&tb, 2, 'A', one, sizeof(one)) == -1, "part 2 rejected");
  check("frag.room", ais_build_static_a(&ta, 0, 'A', frag, 1) == -1, "1 line for a 2-frag msg");

  printf("== channel selection ==\n");
  if (ais_build_static_b(&tb, 0, 'B', one, sizeof(one)) == 1) {
    check("chan.B", strstr(one, ",,B,") != NULL, one);
  } else { check("chan.B", false, "build"); }

  printf("== simulation: seed + motion ==\n");
  AisSim s;
  ais_sim_defaults(&s, 45.255456, -64.500000);
  char msg[64];
  snprintf(msg, sizeof(msg), "%d targets", s.count);
  check("seed.count", s.count == 4, msg);

  const double lat0 = s.targets[0].lat_deg, lon0 = s.targets[0].lon_deg;
  for (int i = 0; i < 60; i++) { ais_sim_tick(&s, 1.0f); }
  /* Target 0 runs at 095 deg -- nearly due east, so longitude climbs and
   * latitude barely moves (slightly south of east). */
  const double dlat = s.targets[0].lat_deg - lat0;
  const double dlon = s.targets[0].lon_deg - lon0;
  snprintf(msg, sizeof(msg), "dlat=%+.5f dlon=%+.5f", dlat, dlon);
  check("motion", dlon > 0.0 && dlat < 0.0, msg);

  /* The stationary Class B must not drift. */
  check("anchored", s.targets[3].sog_kn == 0.0f &&
                    s.targets[3].lat_deg == 45.255456 - 0.018, "yacht held position");

  printf("== simulation: scheduler ==\n");
  /* One minute of ticks: every target should have reported position several
   * times, and no static report is due again until 360 s. */
  AisSim s2;
  ais_sim_defaults(&s2, 45.255456, -64.500000);
  char lines[16][AIS_LINE_MAX];
  int total = 0, statics = 0, bad_cs = 0, too_long = 0;
  for (int sec = 0; sec < 60; sec++) {
    ais_sim_tick(&s2, 1.0f);
    const int got = ais_sim_build_due(&s2, lines, 16);
    for (int i = 0; i < got; i++) {
      /* Checked silently: one PASS line per sentence would bury the result. */
      const char *star = strchr(lines[i], '*');
      unsigned printed = 0;
      if (!star || sscanf(star + 1, "%2x", &printed) != 1 ||
          (uint8_t)printed != ais_checksum(lines[i])) { bad_cs++; }
      if (strlen(lines[i]) > 80u) { too_long++; }
      if (strncmp(lines[i], "!AIVDM,2,", 9) == 0 || strstr(lines[i], ",A,H") != NULL) { statics++; }
    }
    total += got;
  }
  snprintf(msg, sizeof(msg), "%d sentences in 60 s (%d static)", total, statics);
  check("sched.count", total > 20 && total < 200, msg);
  snprintf(msg, sizeof(msg), "%d bad of %d", bad_cs, total);
  check("sched.cs", bad_cs == 0, msg);
  snprintf(msg, sizeof(msg), "%d over 80 chars", too_long);
  check("sched.len", too_long == 0, msg);
  /* Every target must have identified itself inside the first minute, or a
   * plotter shows unnamed contacts: 2 Class A (2 fragments) + 2 Class B (A+B). */
  snprintf(msg, sizeof(msg), "%d static sentences (expect 8)", statics);
  check("sched.static", statics == 8, msg);
  snprintf(msg, sizeof(msg), "%u tracked", (unsigned)s2.sentence_count);
  check("sched.tally", s2.sentence_count == (uint32_t)total, msg);

  printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
