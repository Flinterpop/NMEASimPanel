/*
 * NMEASimPanel -- touchscreen GPS + AIS NMEA simulator for the Elecrow
 * CrowPanel Advance 7.0" (ESP32-S3).
 *
 * The USB-C port (CH340 -> UART0 -> Serial0) is what a PC sees as a COM
 * port; the NMEA stream goes out there at the selected baud. Debug is OFF
 * by default so that COM port carries clean NMEA.
 *
 * GPS ($GP...) and AIS (!AIVDM) share the one link, interleaved -- which is
 * exactly what a real AIS transponder emits, so chart plotters demux it by
 * talker id. That is why the default baud is 38400, the NMEA 0183-HS rate
 * AIS expects, rather than the 4800 of a bare GPS talker.
 *
 * UI: input widgets for the INITIAL lat / lon / altitude / speed / heading,
 * Start / Stop / Reset, and a scrolling log of every sentence as it is sent.
 *
 * Board : esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled
 * Libs  : lvgl, TAMC_GT911, PCA9557 (all in the sketchbook libraries dir)
 */

#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#include "crowpanel_bsp.h"
#include "nmea_sim.h"
#include "ais_sim.h"

/* ---- Configuration ------------------------------------------------ */

#define NMEA_DEBUG     0      /* 1 = also print status to Serial0 (dirties NMEA) */
#define EMIT_PERIOD_MS 1000u  /* 1 Hz */

#if NMEA_DEBUG
  #define DBG(...) Serial0.printf(__VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

/* ---- LVGL plumbing (same pattern as the panel test) --------------- */

static const uint32_t kBufLines = 30;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *pixels) {
  (void)bsp_display_blit(area->x1, area->y1, area->x2, area->y2, pixels);
  lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  static uint16_t         last_x = 0, last_y = 0;
  static lv_indev_state_t state  = LV_INDEV_STATE_RELEASED;

  uint16_t x = 0, y = 0;
  const BspTouch t = bsp_touch_poll(&x, &y);
  if (t == BSP_TOUCH_DOWN) {
    last_x = x; last_y = y; state = LV_INDEV_STATE_PRESSED;
  } else if (t == BSP_TOUCH_UP) {
    state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = (lv_coord_t)last_x;
  data->point.y = (lv_coord_t)last_y;
  data->state   = state;
}

static bool lvgl_begin(void) {
  lv_init();
  const size_t px    = (size_t)BSP_SCREEN_W * kBufLines;
  const size_t bytes = px * sizeof(lv_color_t);
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (b1 == nullptr) { b1 = (lv_color_t *)ps_malloc(bytes); }
  if (b2 == nullptr) { b2 = (lv_color_t *)ps_malloc(bytes); }
  if (b1 == nullptr) { return false; }
  lv_disp_draw_buf_init(&s_draw_buf, b1, b2, px);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res  = BSP_SCREEN_W;
  s_disp_drv.ver_res  = BSP_SCREEN_H;
  s_disp_drv.flush_cb = lvgl_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  if (lv_disp_drv_register(&s_disp_drv) == nullptr) { return false; }

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = lvgl_touch_cb;
  return lv_indev_drv_register(&s_indev_drv) != nullptr;
}

/* ---- Simulator + UI state ----------------------------------------- */

static NmeaSim  g_sim;
static AisSim   g_ais;
static bool     g_running = false;     /* set by START/STOP */
static bool     g_ais_on  = true;      /* AIS traffic on the shared link */
static uint32_t g_enable_mask = 0;     /* bit i = NMEA_SENTENCES[i] enabled */

static const uint32_t kBauds[] = { 4800u, 9600u, 38400u };
/* 38400 is the NMEA 0183-HS rate AIS runs at; a mixed GPS+AIS stream has to
 * use it, and GPS-only receivers cope with the higher rate fine. */
static const uint8_t  kBaudDefaultIdx = 2;   /* 38400 */

static lv_obj_t *ta_lat, *ta_lon, *ta_alt, *ta_spd, *ta_hdg;
static lv_obj_t *g_kb;
static lv_obj_t *g_log;
static lv_obj_t *g_status;
static lv_obj_t *g_btn_start;
static lv_obj_t *g_ais_lbl;

/* Bounded FIFO of log text, trimmed a whole line at a time. */
static char   g_logbuf[4096];
static size_t g_loglen = 0;
static bool   g_log_dirty = false;

static void log_push(const char *text) {
  char line[NMEA_LINE_MAX + 2];
  int n = snprintf(line, sizeof(line), "%s\n", text);
  if (n <= 0) { return; }
  size_t len = (size_t)n;
  if (len >= sizeof(g_logbuf)) { return; }

  while (g_loglen + len >= sizeof(g_logbuf)) {
    char *nl = (char *)memchr(g_logbuf, '\n', g_loglen);
    size_t drop = nl ? (size_t)(nl - g_logbuf) + 1 : g_loglen;
    memmove(g_logbuf, g_logbuf + drop, g_loglen - drop);
    g_loglen -= drop;
  }
  memcpy(g_logbuf + g_loglen, line, len);
  g_loglen += len;
  g_logbuf[g_loglen] = '\0';
  g_log_dirty = true;
}

/* ---- Field <-> sim marshalling ------------------------------------ */

static void fields_to_sim(void) {
  g_sim.lat_deg     = atof(lv_textarea_get_text(ta_lat));
  g_sim.lon_deg     = atof(lv_textarea_get_text(ta_lon));
  g_sim.alt_m       = (float)atof(lv_textarea_get_text(ta_alt));
  g_sim.speed_kn    = (float)atof(lv_textarea_get_text(ta_spd));
  g_sim.heading_deg = (float)atof(lv_textarea_get_text(ta_hdg));

  /* Clamp to sane ranges rather than trust raw typing. */
  if (g_sim.lat_deg >  90.0) { g_sim.lat_deg =  90.0; }
  if (g_sim.lat_deg < -90.0) { g_sim.lat_deg = -90.0; }
  if (g_sim.lon_deg > 180.0) { g_sim.lon_deg = 180.0; }
  if (g_sim.lon_deg < -180.0) { g_sim.lon_deg = -180.0; }
  if (g_sim.speed_kn < 0.0f)  { g_sim.speed_kn = 0.0f; }
  while (g_sim.heading_deg >= 360.0f) { g_sim.heading_deg -= 360.0f; }
  while (g_sim.heading_deg <    0.0f) { g_sim.heading_deg += 360.0f; }
}

static void defaults_to_fields(void) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.6f", g_sim.lat_deg);     lv_textarea_set_text(ta_lat, buf);
  snprintf(buf, sizeof(buf), "%.6f", g_sim.lon_deg);     lv_textarea_set_text(ta_lon, buf);
  snprintf(buf, sizeof(buf), "%.1f", (double)g_sim.alt_m);       lv_textarea_set_text(ta_alt, buf);
  snprintf(buf, sizeof(buf), "%.1f", (double)g_sim.speed_kn);    lv_textarea_set_text(ta_spd, buf);
  snprintf(buf, sizeof(buf), "%.1f", (double)g_sim.heading_deg); lv_textarea_set_text(ta_hdg, buf);
}

/* ---- Event handlers ----------------------------------------------- */

static void field_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(g_kb, ta);
    lv_obj_clear_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_kb);
  } else if (code == LV_EVENT_DEFOCUSED) {
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

static void kb_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    /* Drop focus so the same field can be re-tapped to reopen the keypad. */
    lv_obj_t *ta = lv_keyboard_get_textarea(g_kb);
    if (ta) { lv_obj_clear_state(ta, LV_STATE_FOCUSED); }
  }
}

static void set_running(bool run) {
  g_running = run;
  lv_obj_t *lbl = lv_obj_get_child(g_btn_start, 0);
  if (lbl) { lv_label_set_text(lbl, run ? "RUNNING" : "START"); }
  lv_obj_set_style_bg_color(g_btn_start,
      lv_palette_main(run ? LV_PALETTE_GREEN : LV_PALETTE_BLUE), 0);
}

static void start_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) { return; }
  fields_to_sim();
  /* Re-seed the traffic picture around wherever own ship now is, so targets
   * appear on screen instead of thousands of miles away. */
  ais_sim_defaults(&g_ais, g_sim.lat_deg, g_sim.lon_deg);
  set_running(true);
  log_push("--- START ---");
}

static void stop_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) { return; }
  set_running(false);
  log_push("--- STOP ---");
}

static void reset_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) { return; }
  set_running(false);
  nmea_sim_defaults(&g_sim);
  ais_sim_defaults(&g_ais, g_sim.lat_deg, g_sim.lon_deg);
  defaults_to_fields();
  g_loglen = 0; g_logbuf[0] = '\0'; g_log_dirty = true;
  log_push("--- RESET ---");
}

static void toggle_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) { return; }
  lv_obj_t *sw = lv_event_get_target(e);
  const int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= NMEA_SENTENCE_COUNT) { return; }
  if (lv_obj_has_state(sw, LV_STATE_CHECKED)) { g_enable_mask |=  (1u << idx); }
  else                                        { g_enable_mask &= ~(1u << idx); }
}

static void ais_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) { return; }
  lv_obj_t *sw = lv_event_get_target(e);
  g_ais_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  log_push(g_ais_on ? "--- AIS on ---" : "--- AIS off ---");
}

static void baud_event(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) { return; }
  lv_obj_t *dd = lv_event_get_target(e);
  const uint16_t idx = lv_dropdown_get_selected(dd);
  if (idx >= (sizeof(kBauds) / sizeof(kBauds[0]))) { return; }
  Serial0.flush();
  Serial0.updateBaudRate(kBauds[idx]);
  char m[32];
  snprintf(m, sizeof(m), "--- baud %lu ---", (unsigned long)kBauds[idx]);
  log_push(m);
}

/* ---- UI construction ---------------------------------------------- */

static lv_obj_t *make_field(lv_obj_t *parent, const char *label, lv_coord_t y,
                            const char *initial) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, label);
  lv_obj_set_pos(lbl, 0, y + 9);

  /* Explicit height: the default content-sized textarea is tall enough that
   * five of them overflow the form. */
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_text(ta, initial);
  lv_obj_set_size(ta, 190, 34);
  lv_obj_set_style_pad_ver(ta, 4, 0);
  lv_obj_set_pos(ta, 110, y);
  lv_obj_add_event_cb(ta, field_event, LV_EVENT_ALL, nullptr);
  return ta;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_coord_t x,
                             lv_coord_t y, lv_palette_t color,
                             lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 95, 56);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_bg_color(btn, lv_palette_main(color), 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return btn;
}

static void build_sentence_toggles(lv_obj_t *scr) {
  lv_obj_t *box = lv_obj_create(scr);
  /* 108 px: header at 0, then 2 rows of 36 ending at 88, inside the 92 px of
   * content left after padding. Shortened from 148 to make room for the AIS
   * panel below; a 5th sentence would push this back to 3 rows (144). */
  lv_obj_set_size(box, 320, 108);
  lv_obj_set_pos(box, 12, 238);
  lv_obj_set_style_pad_all(box, 8, 0);

  lv_obj_t *hdr = lv_label_create(box);
  lv_label_set_text(hdr, "Sentences");
  lv_obj_set_pos(hdr, 0, 0);

  /* One toggle per registry row, so new sentences appear here for free.
   * Two columns keeps 6 visible without scrolling; beyond that the box
   * scrolls vertically. */
  for (int i = 0; i < NMEA_SENTENCE_COUNT; i++) {
    const int col = i % 2;
    const int row = i / 2;
    const lv_coord_t x = (lv_coord_t)(4 + col * 152);
    const lv_coord_t y = (lv_coord_t)(26 + row * 36);

    lv_obj_t *sw = lv_switch_create(box);
    lv_obj_set_size(sw, 50, 26);
    lv_obj_set_pos(sw, x, y);
    lv_obj_add_state(sw, LV_STATE_CHECKED);   /* matches default mask */
    lv_obj_add_event_cb(sw, toggle_event, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, NMEA_SENTENCES[i].name);
    lv_obj_set_pos(lbl, (lv_coord_t)(x + 60), (lv_coord_t)(y + 6));
  }
}

/* AIS enable switch plus a live target/sentence readout. Occupies the strip
 * between the sentence toggles (ends 346) and the buttons (start 392). */
static void build_ais_panel(lv_obj_t *scr) {
  lv_obj_t *box = lv_obj_create(scr);
  lv_obj_set_size(box, 320, 38);
  lv_obj_set_pos(box, 12, 350);
  lv_obj_set_style_pad_all(box, 6, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *sw = lv_switch_create(box);
  lv_obj_set_size(sw, 50, 26);
  lv_obj_set_pos(sw, 0, 0);
  if (g_ais_on) { lv_obj_add_state(sw, LV_STATE_CHECKED); }
  lv_obj_add_event_cb(sw, ais_event, LV_EVENT_VALUE_CHANGED, nullptr);

  g_ais_lbl = lv_label_create(box);
  lv_obj_set_pos(g_ais_lbl, 60, 5);
  lv_label_set_text(g_ais_lbl, "AIS");
}

static void build_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "NMEASimPanel  --  GPS + AIS");
  lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), 0);
  lv_obj_set_pos(title, 12, 10);

  /* Baud selector, top area. */
  lv_obj_t *baud_lbl = lv_label_create(scr);
  lv_label_set_text(baud_lbl, "Baud");
  lv_obj_set_style_text_color(baud_lbl, lv_color_hex(0xE0E0E0), 0);
  lv_obj_set_pos(baud_lbl, 190, 10);

  lv_obj_t *baud_dd = lv_dropdown_create(scr);
  lv_dropdown_set_options(baud_dd, "4800\n9600\n38400");
  lv_dropdown_set_selected(baud_dd, kBaudDefaultIdx);
  lv_obj_set_width(baud_dd, 90);
  lv_obj_set_pos(baud_dd, 240, 2);
  lv_obj_add_event_cb(baud_dd, baud_event, LV_EVENT_VALUE_CHANGED, nullptr);

  /* Left: initial-condition input form. */
  lv_obj_t *form = lv_obj_create(scr);
  lv_obj_set_size(form, 320, 196);
  lv_obj_set_pos(form, 12, 38);
  lv_obj_clear_flag(form, LV_OBJ_FLAG_SCROLLABLE);
  /* Trim the default container padding so the first field starts at the top. */
  lv_obj_set_style_pad_top(form, 4, 0);
  lv_obj_set_style_pad_bottom(form, 4, 0);
  lv_obj_set_style_pad_left(form, 10, 0);

  /* 5 rows of 36 px: last row ends at 144 + 34 = 178, inside the 188 px of
   * content height left after padding. */
  ta_lat = make_field(form, "Lat (deg)",  0,   "45.255456");
  ta_lon = make_field(form, "Lon (deg)",  36,  "-64.500000");
  ta_alt = make_field(form, "Alt (m)",    72,  "101.3");
  ta_spd = make_field(form, "Speed (kn)", 108, "150.1");
  ta_hdg = make_field(form, "Heading",    144, "45.1");

  build_sentence_toggles(scr);
  build_ais_panel(scr);

  /* Buttons across the bottom of the left column. */
  g_btn_start = make_button(scr, "START",  12,  392, LV_PALETTE_BLUE, start_event);
  make_button(scr, "STOP",  120, 392, LV_PALETTE_RED,  stop_event);
  make_button(scr, "RESET", 228, 392, LV_PALETTE_GREY, reset_event);

  /* Right: monospace status line + scrolling sent-sentence log. */
  g_status = lv_label_create(scr);
  lv_obj_set_style_text_color(g_status, lv_color_hex(0x7FE0A0), 0);
  lv_obj_set_style_text_font(g_status, &lv_font_unscii_8, 0);   /* small monospace */
  lv_obj_set_pos(g_status, 348, 12);
  lv_label_set_text(g_status, "stopped");

  g_log = lv_textarea_create(scr);
  lv_obj_set_size(g_log, 440, 392);
  lv_obj_set_pos(g_log, 348, 44);
  lv_textarea_set_text(g_log, "");
  lv_obj_add_state(g_log, LV_STATE_DISABLED);   /* read-only display */
  lv_obj_set_style_text_font(g_log, &lv_font_montserrat_14, 0);

  /* Shared numeric keyboard, hidden until a field is focused. */
  g_kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(g_kb, BSP_SCREEN_W, 200);
  lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_kb, kb_event, LV_EVENT_ALL, nullptr);
}

/* ---- Periodic UI refresh ------------------------------------------ */

static void ui_timer(lv_timer_t *t) {
  (void)t;
  if (g_log_dirty) {
    lv_textarea_set_text(g_log, g_logbuf);
    lv_obj_scroll_to_y(g_log, LV_COORD_MAX, LV_ANIM_OFF);  /* stick to bottom */
    g_log_dirty = false;
  }
  char s[112];
  snprintf(s, sizeof(s), "%s  %02u:%02u:%02u  gps=%lu ais=%lu  %.5f, %.5f",
           g_running ? "RUNNING" : "stopped",
           g_sim.hour, g_sim.minute, g_sim.second,
           (unsigned long)g_sim.sentence_count,
           (unsigned long)g_ais.sentence_count,
           g_sim.lat_deg, g_sim.lon_deg);
  lv_label_set_text(g_status, s);

  if (g_ais_lbl != nullptr) {
    char a[48];
    snprintf(a, sizeof(a), "AIS  %d targets", g_ais.count);
    lv_label_set_text(g_ais_lbl, a);
  }
}

/* ---- Emit ---------------------------------------------------------- */

/* One 1 Hz slot: GPS first, then whatever AIS traffic is due. Both go out the
 * same link interleaved, which is what a real transponder does -- consumers
 * demux by talker id.
 *
 * The line buffers are static: together they are ~2 KB, which is more than
 * this task's stack should carry on every tick.
 *
 * With AIS switched off the target world is frozen rather than advanced
 * silently, so re-enabling resumes smoothly instead of dumping a burst of
 * simultaneously-overdue reports. */
static void emit_once(void) {
  static char gps_lines[8][NMEA_LINE_MAX];
  static char ais_lines[12][AIS_LINE_MAX];

  nmea_sim_tick(&g_sim, (float)EMIT_PERIOD_MS / 1000.0f);
  const int n = nmea_sim_build_enabled(&g_sim, g_enable_mask, gps_lines, 8);
  for (int i = 0; i < n; i++) {
    Serial0.print(gps_lines[i]);
    Serial0.print("\r\n");
    log_push(gps_lines[i]);
    g_sim.sentence_count++;
  }

  if (!g_ais_on) { return; }

  ais_sim_tick(&g_ais, (float)EMIT_PERIOD_MS / 1000.0f);
  const int m = ais_sim_build_due(&g_ais, ais_lines, 12);
  for (int i = 0; i < m; i++) {
    Serial0.print(ais_lines[i]);
    Serial0.print("\r\n");
    log_push(ais_lines[i]);   /* ais_sim_build_due already tallied these */
  }
}

/* ------------------------------------------------------------------- */

void setup(void) {
  Serial0.begin(kBauds[kBaudDefaultIdx]);
  nmea_sim_defaults(&g_sim);
  ais_sim_defaults(&g_ais, g_sim.lat_deg, g_sim.lon_deg);

  /* Enable every registered sentence by default (matches the toggles). */
  g_enable_mask = (NMEA_SENTENCE_COUNT >= 32)
                      ? 0xFFFFFFFFu
                      : ((1u << NMEA_SENTENCE_COUNT) - 1u);

  if (!bsp_display_begin()) { return; }
  bsp_touch_begin();
  if (!lvgl_begin())        { return; }

  build_ui();
  lv_timer_create(ui_timer, 250, nullptr);
  lv_timer_handler();
  bsp_backlight_set(255);
}

void loop(void) {
  static uint32_t last_emit = 0;
  lv_timer_handler();

  const uint32_t now = millis();
  if (g_running && (now - last_emit) >= EMIT_PERIOD_MS) {
    last_emit = now;
    emit_once();
  }
  delay(5);
}
