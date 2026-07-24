#include "crowpanel_bsp.h"

#include <assert.h>
#include <Wire.h>
#include <PCA9557.h>
#include <TAMC_GT911.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>

/* ---- Panel timings and pin map, from the vendor LovyanGFX config for
 * this exact panel (LGFX_Elecrow_ESP32_Display_WZ8048C070.h). ---- */

static const uint32_t kPclkHz = 12000000u;

static const int kPinHsync = 39;
static const int kPinVsync = 40;
static const int kPinDe    = 41;
static const int kPinPclk  = 0;
static const int kPinBl    = 2;

/* Order is B0..B4, G0..G5, R0..R4 -- matches esp_lcd's data_gpio_nums. */
static const int kPinData[16] = {
  15,  7,  6,  5,  4,      /* B0..B4 */
   9, 46,  3,  8, 16,  1,  /* G0..G5 */
  14, 21, 47, 48, 45       /* R0..R4 */
};

static const uint8_t kI2cSda = 19;
static const uint8_t kI2cScl = 20;

static const uint8_t kGt911AddrA = 0x5D;  /* this unit */
static const uint8_t kGt911AddrB = 0x14;  /* what the vendor config claims */

static esp_lcd_panel_handle_t s_panel      = nullptr;
static uint8_t                s_touch_addr = 0;

/* ------------------------------------------------------------------ */
/* Backlight                                                           */
/* ------------------------------------------------------------------ */

void bsp_backlight_set(uint8_t level) {
  ledcWrite(kPinBl, level);
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

bool bsp_display_begin(void) {
  if (!ledcAttach(kPinBl, 5000u, 8u)) { return false; }
  bsp_backlight_set(0);

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src        = LCD_CLK_SRC_PLL160M;
  cfg.data_width     = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs        = 1;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = kPinHsync;
  cfg.vsync_gpio_num = kPinVsync;
  cfg.de_gpio_num    = kPinDe;
  cfg.pclk_gpio_num  = kPinPclk;
  cfg.disp_gpio_num  = -1;
  for (uint8_t i = 0; i < 16; i++) { cfg.data_gpio_nums[i] = kPinData[i]; }

  cfg.timings.pclk_hz           = kPclkHz;
  cfg.timings.h_res             = BSP_SCREEN_W;
  cfg.timings.v_res             = BSP_SCREEN_H;
  cfg.timings.hsync_pulse_width = 48;
  cfg.timings.hsync_back_porch  = 40;
  cfg.timings.hsync_front_porch = 40;
  cfg.timings.vsync_pulse_width = 31;
  cfg.timings.vsync_back_porch  = 13;
  cfg.timings.vsync_front_porch = 1;
  cfg.timings.flags.pclk_active_neg = 1;

  cfg.flags.fb_in_psram = 1;

  if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) { return false; }
  if (esp_lcd_panel_reset(s_panel) != ESP_OK) { return false; }
  if (esp_lcd_panel_init(s_panel) != ESP_OK)  { return false; }
  return true;
}

bool bsp_display_blit(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      const void *pixels) {
  assert(pixels != nullptr);
  if (s_panel == nullptr) { return false; }
  /* esp_lcd takes an exclusive end coordinate; LVGL gives an inclusive one. */
  return esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1,
                                   pixels) == ESP_OK;
}

/* ------------------------------------------------------------------ */
/* GT911 touch                                                         */
/* ------------------------------------------------------------------ */
/*
 * Two things are required to get this controller scanning, and BOTH are
 * easy to miss because the chip answers I2C and reports a sane product ID
 * and resolution without either of them:
 *
 * 1. A hardware reset via the PCA9557 expander (IO0 = TP_RST, IO1 = TP_INT).
 *    TP_RST otherwise floats high through its pull-up, so the chip powers up
 *    but never gets a clean reset. Holding TP_INT low during reset is what
 *    latches the I2C address to 0x5D.
 * 2. A config reflash. This unit reports config version 0xFF (invalid), and
 *    until a recalculated checksum plus the CONFIG_FRESH flag are written it
 *    never starts its scan engine -- 0x814E stays at 0x00 forever no matter
 *    how you poll it. TAMC_GT911::begin() does this inside setResolution().
 *
 * Skipping (2) was why hand-rolled register polling looked completely dead.
 */

static PCA9557   s_expander;
static TAMC_GT911 s_ts(kI2cSda, kI2cScl, (uint8_t)-1, (uint8_t)-1,
                       BSP_SCREEN_W, BSP_SCREEN_H);

/* Vendor reset order and delays, from the course example
 * "7.0 v3.0 touch new code/4/crowpanel-esp32-7.0-3.0-touch". */
static void touch_hw_reset(void) {
  pinMode(38, OUTPUT);
  digitalWrite(38, LOW);

  s_expander.reset();
  s_expander.setMode(IO_OUTPUT);
  s_expander.setState(IO0, IO_LOW);   /* TP_RST asserted */
  s_expander.setState(IO1, IO_LOW);   /* TP_INT low -> latch address 0x5D */
  delay(20);
  s_expander.setState(IO0, IO_HIGH);  /* release reset */
  delay(100);
  s_expander.setMode(IO1, IO_INPUT);  /* INT back to input */
  delay(50);
}

bool bsp_touch_begin(void) {
  Wire.begin(kI2cSda, kI2cScl);
  touch_hw_reset();

  Wire.beginTransmission(kGt911AddrA);
  s_touch_addr = (Wire.endTransmission() == 0) ? kGt911AddrA : kGt911AddrB;

  s_ts.begin(s_touch_addr);
  /* ROTATION_INVERTED is the identity transform in readPoint(). Measured on
   * this unit: x spans 9..793 and y spans 4..475 left-to-right, top-to-bottom,
   * so no inversion or axis swap is wanted. */
  s_ts.setRotation(ROTATION_INVERTED);
  return true;
}

uint8_t bsp_touch_addr(void) { return s_touch_addr; }

BspTouch bsp_touch_poll(uint16_t *x, uint16_t *y) {
  assert(x != nullptr);
  assert(y != nullptr);

  /* Require two consecutive empty reads before reporting a release, so a
   * single skipped scan cycle cannot break a drag. */
  static uint8_t release_run = 0;

  s_ts.read();

  if (s_ts.isTouched && s_ts.touches > 0) {
    release_run = 0;
    int32_t px = s_ts.points[0].x;
    int32_t py = s_ts.points[0].y;
    /* Clamp rather than discard: silently dropping out-of-range points is
     * indistinguishable from broken hardware. */
    if (px < 0) { px = 0; }
    if (py < 0) { py = 0; }
    if (px > BSP_SCREEN_W - 1) { px = BSP_SCREEN_W - 1; }
    if (py > BSP_SCREEN_H - 1) { py = BSP_SCREEN_H - 1; }
    *x = (uint16_t)px;
    *y = (uint16_t)py;
    return BSP_TOUCH_DOWN;
  }

  if (release_run < 2) {
    release_run++;
    return BSP_TOUCH_NONE;
  }
  return BSP_TOUCH_UP;
}
