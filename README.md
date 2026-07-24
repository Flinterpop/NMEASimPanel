# NMEA_Sim

A touchscreen NMEA-0183 simulator for the **Elecrow CrowPanel Advance 7.0"**
(ESP32-S3, 800x480 RGB panel with GT911 capacitive touch).

It generates GPS sentences with dead-reckoned motion and streams them out the
USB-C port, which a PC sees as an ordinary COM port — so any chart plotter,
logger, or test harness can consume it as if it were a real GPS receiver.

This is the touchscreen successor to the headless
[`ad_GPS_SerialSim`](https://github.com/Flinterpop/ad_GPS_SerialSim), which
emitted `$GPRMC` + `$GPGGA` at 1 Hz with no UI.

---

## Features

- **GPS sentences**: `$GPRMC`, `$GPGGA`, `$GPVTG`, `$GPGLL` at 1 Hz
- **Per-sentence enable/disable** — one toggle each
- **Initial-condition entry** via on-screen numeric keypad: latitude,
  longitude, altitude, speed, heading
- **Selectable baud**: 4800 / 9600 / 38400, switchable live
- **Live scrolling log** of every sentence as it is sent
- **Dead-reckoned motion** — position advances from speed and heading each tick

Planned: **AIS** (the sentence registry is already structured for it).

---

## Hardware

| Item | Detail |
|---|---|
| Board | Elecrow CrowPanel Advance 7.0" (WZ8048C070) |
| MCU | ESP32-S3-WROOM-1-N4R8 — 4 MB flash, 8 MB **octal** PSRAM |
| Display | 800x480 RGB565, driven by the IDF `esp_lcd` RGB panel driver |
| Touch | GT911 over I2C at **0x5D** (SDA 19 / SCL 20) |
| Backlight | GPIO 2, LEDC PWM |
| Serial out | UART0 -> CH340 -> USB-C (`Serial0`) |

### Serial / COM port

NMEA goes out **`Serial0`**, which is the CH340 behind the USB-C connector —
the port your PC enumerates. Debug output is compiled out by default
(`NMEA_DEBUG 0`) so the stream stays clean NMEA.

> On this board `Serial` is USB-CDC and is **not wired out**. Anything printed
> to `Serial` goes nowhere. Always use `Serial0`.

---

## Building

**FQBN**

```
esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled
```

**Libraries** (all in the Arduino sketchbook `libraries/` folder):

- `lvgl` 8.3.3 — plus `lv_conf.h` sitting *beside* the `lvgl` folder, not
  inside it (that is where `lv_conf_internal.h`'s `../../lv_conf.h` resolves)
- `TAMC_GT911` — touch controller
- `PCA9557` — I/O expander used for the touch reset line

Command line:

```sh
arduino-cli compile --fqbn esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled .
arduino-cli upload -p COM25 --fqbn esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled .
```

Footprint: ~42% flash, ~24% RAM.

### Required board-definition fix (PSRAM)

The stock `elecrow_crowpanel_7` entry in the ESP32 core's `boards.txt` sets
`psram_type=opi` but **never sets `build.memory_type`**, so it falls back to
`{build.boot}_qspi` and links the *quad* PSRAM driver against this board's
*octal* PSRAM chip. The result:

```
E (116) quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
```

PSRAM then reports 0 MB, and since the 800x480 framebuffer lives in PSRAM the
display never comes up. Fix by creating `boards.local.txt` next to `boards.txt`
in the core directory
(`.../packages/esp32/hardware/esp32/<ver>/boards.local.txt`):

```
elecrow_crowpanel_7.build.memory_type={build.boot}_{build.psram_type}
elecrow_crowpanel_7.menu.PSRAM.disabled.build.psram_type=qspi
```

**This file is lost whenever the ESP32 core is upgraded or reinstalled.** If
the display suddenly stops working after a core update, check PSRAM first.

---

## Using it

1. Flash and power the panel. It boots **stopped** — nothing is transmitted
   until you press START.
2. Tap a field to bring up the numeric keypad; set the initial latitude,
   longitude, altitude, speed and heading.
3. Toggle the sentences you want.
4. Pick the baud rate your PC application expects.
5. Press **START**. Sentences stream out the USB-C port at 1 Hz and appear in
   the log.

| Control | Effect |
|---|---|
| START | Applies the entered fields as initial conditions and begins transmitting |
| STOP | Halts transmission and motion; position is retained |
| RESET | Stops, restores defaults, clears the log |

The green status line shows run state, simulated UTC, sentence count, and the
current position.

### Example output

```
$GPRMC,204312,A,4515.6805,N,06429.4965,W,150.1,45.1,230726,003.1,W*4C
$GPGGA,204312,4515.6805,N,06429.4965,W,1,09,0.9,101.3,M,23.1,M,,*5C
$GPVTG,45.1,T,48.2,M,150.1,N,278.0,K,A*25
$GPGLL,4515.6805,N,06429.4965,W,204312,A,A*56
```

---

## Architecture

| File | Role |
|---|---|
| `NMEA_Sim.ino` | LVGL user interface and the 1 Hz emit loop |
| `nmea_sim.h/.cpp` | Simulator core: state, motion, sentence builders. **No Arduino dependency** — compiles and runs on a PC |
| `crowpanel_bsp.h/.cpp` | Board support: RGB panel, GT911 touch, backlight |
| `test/test_nmea.cpp` | Host-side test for the simulator core |

Keeping the core free of Arduino headers means sentence formatting and motion
can be tested on a desktop compiler, where iteration is fast and failures are
visible — no hardware in the loop.

### Adding a sentence

The sentence set is data-driven. Add a builder, then one row in the registry:

```c
/* nmea_sim.cpp */
int nmea_build_gsa(const NmeaSim *s, char *out, size_t cap) { ... }

const NmeaSentenceDef NMEA_SENTENCES[] = {
  { "RMC", nmea_build_rmc },
  { "GGA", nmea_build_gga },
  { "VTG", nmea_build_vtg },
  { "GLL", nmea_build_gll },
  { "GSA", nmea_build_gsa },   /* <-- new */
};
```

That is the whole change. The UI grows an enable/disable toggle for it and the
emit mask picks up a bit for it automatically — no UI code to touch.

**Append, don't insert**: enable-mask bit *i* corresponds to registry row *i*,
so inserting in the middle renumbers the existing bits.

### Motion model

Straight-line dead reckoning: each tick advances position by
`speed x dt` along the current heading, converting metres to degrees with
111120 m per degree of latitude and a `cos(latitude)` correction for longitude.

`turn_rate_dps` exists in the state and is 0 by default. Set it non-zero for
the circular track the original `ad_GPS_SerialSim` produced; it is not exposed
in the UI yet.

---

## Host-side tests

The simulator core builds with any desktop C++ compiler. With MSVC:

```bat
test\build_test.bat
```

This validates sentence formats, recomputes every checksum, exercises the
enable mask, and checks motion direction and clock rollover.

```
[PASS] RMC  $GPRMC,204300,A,4515.3274,N,06430.0000,W,150.1,45.1,230726,003.1,W*40   (cs 40 ok)
[PASS] GGA  $GPGGA,204300,4515.3274,N,06430.0000,W,1,09,0.9,101.3,M,23.1,M,,*50   (cs 50 ok)
[PASS] VTG  $GPVTG,45.1,T,48.2,M,150.1,N,278.0,K,A*25   (cs 25 ok)
[PASS] GLL  $GPGLL,4515.3274,N,06430.0000,W,204300,A,A*56   (cs 56 ok)
ALL PASS (0 failures)
```

---

## Known issues and gotchas

**A PC opening the COM port may reset the board.** RTS drives EN and DTR drives
GPIO0 through the auto-reset circuit, so terminal software that asserts those
lines on connect will restart the simulator. Most applications have a setting
to leave them alone.

**Boot banner baud mismatch.** The ROM bootloader always prints at 115200
regardless of the selected NMEA baud, so a PC listening at 4800/9600/38400 sees
one burst of garbage at power-on, then clean NMEA. Cosmetic.

**Never drive GPIO 26-32 or 33-37.** GPIO 26-32 are the SPI flash bus and
33-37 are the octal PSRAM bus. Driving any of them stalls the flash cache and
the board boot-loops forever on `TG1WDT_SYS_RST`. The variant's
`pins_arduino.h` lists G33..G38 as though they were free; with PSRAM enabled
they are not.

**GT911 touch needs two steps to start scanning**, and it reports a healthy
product ID and resolution without either — so an I2C probe alone proves
nothing:

1. A hardware reset via the PCA9557 expander at 0x18 (IO0 = reset,
   IO1 = interrupt, held low during reset to latch address 0x5D). The reset
   line otherwise just floats high on its pull-up.
2. A configuration reflash. This panel reports config version `0xFF`, and
   until a recalculated checksum plus the `CONFIG_FRESH` flag are written, the
   status register stays `0x00` forever.

`TAMC_GT911::begin()` handles the second; `crowpanel_bsp.cpp` does the first.

**LovyanGFX is not used.** The vendor-bundled LovyanGFX 1.1.16 does not compile
against the core's IDF 5.5 (renamed `lcd_periph_signals`,
`gpio_hal_iomux_func_sel`, and a changed `i2c_signal_conn_t`). The display is
driven with the IDF `esp_lcd` RGB driver that ships inside the Arduino core
instead.

---

## Roadmap

- AIS sentence generation (`!AIVDM`) — next
- Additional GPS sentences (GSA, GSV)
- Optional turn rate control in the UI for circular tracks
