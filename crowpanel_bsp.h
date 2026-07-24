/*
 * crowpanel_bsp.h -- minimal board support for the Elecrow CrowPanel
 * Advance 7.0" (WZ8048C070, ESP32-S3-WROOM-1-N4R8).
 *
 * Display  : ESP-IDF esp_lcd RGB panel driver (bundled with the Arduino core).
 *            LovyanGFX 1.1.16 does NOT compile against the core's IDF 5.5.
 * Touch    : GT911 over I2C, addressed directly (no library).
 * Backlight: GPIO 2 via LEDC PWM.
 *
 * ---------------------------------------------------------------------
 * PIN SAFETY: never drive GPIO 26-32 (SPI flash) or GPIO 33-37 (octal
 * PSRAM). Touching one hangs the flash cache and the board boot-loops
 * forever on TG1WDT_SYS_RST. The variant pins_arduino.h lists G33..G38 as
 * if free; with PSRAM=enabled they are not.
 * ---------------------------------------------------------------------
 *
 * Build: esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled
 * Requires the boards.local.txt fix that sets build.memory_type, otherwise
 * the core links the quad-PSRAM driver against this octal chip and PSRAM
 * comes up as 0 MB (which also kills the framebuffer).
 */

#ifndef CROWPANEL_BSP_H
#define CROWPANEL_BSP_H

#include <Arduino.h>

static const uint16_t BSP_SCREEN_W = 800;
static const uint16_t BSP_SCREEN_H = 480;

enum BspTouch : uint8_t {
  BSP_TOUCH_NONE = 0, /* no fresh report from the controller */
  BSP_TOUCH_UP   = 1, /* fresh report: nothing pressed */
  BSP_TOUCH_DOWN = 2  /* fresh report: pressed, x/y valid */
};

/* Brings up LEDC, the RGB panel and its PSRAM framebuffer. Leaves the
 * backlight off so nothing flashes before the first frame is drawn. */
bool bsp_display_begin(void);

/* Blits a rectangle. x2/y2 are INCLUSIVE (LVGL's convention); the
 * exclusive-end conversion esp_lcd wants happens inside. */
bool bsp_display_blit(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      const void *pixels);

void bsp_backlight_set(uint8_t level);

/* Starts I2C and probes for the GT911 at 0x5D then 0x14. Note this unit
 * answers at 0x5D even though the vendor LovyanGFX config says 0x14. */
bool bsp_touch_begin(void);

uint8_t bsp_touch_addr(void);

/* Polls the controller. Returns BSP_TOUCH_NONE when there is no new report,
 * so callers can latch the previous state rather than seeing a phantom
 * release while a finger is held still. */
BspTouch bsp_touch_poll(uint16_t *x, uint16_t *y);

#endif /* CROWPANEL_BSP_H */
