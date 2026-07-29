# NMEASimPanel

A touchscreen NMEA-0183 simulator for the **Elecrow CrowPanel Advance 7.0"**
(ESP32-S3, 800x480 RGB panel with GT911 capacitive touch).
https://www.elecrow.com/crowpanel-advance-7-hmi-esp32-ai-display-800x480-ai-ips-touch-screen.html

https://www.amazon.ca/ELECROW-Dual-Core-Compatible-PlatformIO-MicroPython/dp/B0F8NFFH29?pd_rd_w=vGprm&content-id=amzn1.sym.128d8954-9caa-4387-9adf-38f1702b31a1&pf_rd_p=128d8954-9caa-4387-9adf-38f1702b31a1&pf_rd_r=XAMC2Z730PK0EFAN4H5K&pd_rd_wg=DsLqO&pd_rd_r=49bc8fb4-2989-4309-bb54-fa90ca3cfaff&pd_rd_i=B0F8NFFH29&th=1

It generates GPS sentences with dead-reckoned motion and streams them out the
USB-C port, which a PC sees as an ordinary COM port — so any chart plotter,
logger, or test harness can consume it as if it were a real GPS receiver.

This is the touchscreen successor to the headless

[`ad_GPS_SerialSim`](https://github.com/Flinterpop/ad_GPS_SerialSim), which
emitted `$GPRMC` + `$GPGGA` at 1 Hz with no UI.

---

<img width="3718" height="2594" alt="NMEA_simpanel" src="https://github.com/user-attachments/assets/1ec80002-a740-4663-8555-751e5ba5e1ca" />

---

## Features

- **GPS sentences**: `$GPRMC`, `$GPGGA`, `$GPVTG`, `$GPGLL` at 1 Hz
- **AIS traffic**: `!AIVDM` messages for a small table of simulated targets —
  types **1** (Class A position), **18** (Class B position), **5** (static and
  voyage data, two fragments) and **24** A/B (Class B static data)
- **AIS playback** — replay a recorded `.ais` capture instead of the simulated
  targets, chosen from a dropdown, **at the rate it was recorded at**, with a
  1x / 4x / 16x / 60x speed selector. GPS keeps generating either way
- **Per-sentence enable/disable** — one toggle each, plus an AIS on/off switch
- **Initial-condition entry** via on-screen numeric keypad: latitude,
  longitude, altitude, speed, heading
- **Selectable baud**: 4800 / 9600 / 38400, switchable live
- **Live scrolling log** of every sentence as it is sent
- **Dead-reckoned motion** — own ship and every AIS target advance each tick

### GPS and AIS share one link

Both streams go out the same COM port, interleaved — which is what a real AIS
transponder emits, so chart plotters demux them by talker id. That is why the
**default baud is 38400**, the NMEA 0183-HS rate AIS runs at, rather than the
4800 of a bare GPS talker.

If an application insists on two separate inputs, split the one port PC-side
on the leading character (`$` for GPS, `!` for AIS) into two virtual COM ports,
or bridge to UDP. See `board_eval_waveshare_esp32s3_touch_lcd_7.md` for why the
alternative — hardware with two USB ports — is not needed for this.

AIS reporting rates are simplified from ITU-R M.1371's TDMA schedule: Class A
position every 3 s under way (10 s when slow), Class B every 30 s, and static
data every 360 s, with targets staggered so they do not all report at once.

### AIS source: simulated or recorded

The dropdown in the AIS panel selects the live simulator or any capture in
`AIS_Recordings/`. Playback substitutes **only** the AIS source — GPS keeps
generating, so a plotter never sees two conflicting own-ship positions.

Recordings are compiled into the sketch as flash constants rather than read
from the SD slot, which is unproven on this board: the SPI pins are inferred
rather than vendor-confirmed and the slot has never been exercised. Regenerate
the header after adding or changing a capture:

```powershell
pwsh tools/make_ais_log.ps1
```

`ais_log.h` is gitignored as a build artifact — it only duplicates bytes the
captures already hold. The sketch builds without it via `__has_include`;
playback is simply unavailable then. Three logs cost about 310 KB of flash,
taking the sketch from 43% to 64% of the partition.

Lines are replayed verbatim and never interpreted, which is what keeps
multi-fragment messages intact.

**Playback reproduces the rate the capture was recorded at.** The logs carry no
per-line timestamps, so the timeline is reconstructed from the UTC inside their
base-station (type 4) messages: `ais_play` recovers those as anchors and
interpolates every message's time by index between them. `AIS_NanooseToVictoria`
yields 181 usable anchors spanning 142 minutes, and replaying it takes 8552 s
against a recovered span of 8551 s.

Two details that came from real bugs in AIS_Streamer's replay and are preserved
here: anchors are filtered to the longest non-decreasing run, because captures
mix base stations whose clocks disagree and the outliers make playback jump
backwards; and messages outside the anchor span are clamped to the boundary
rather than extrapolated, which otherwise overshoots the true duration several
times over.

Speed is selectable in the panel — **1x / 4x / 16x / 60x** — and applies to a
running log without restarting it or skipping messages, since only the gap to
the *next* message is recomputed. At 60x that 142-minute capture replays in
about two and a half minutes.

A log with fewer than two usable anchors, or a zero span, falls back to a fixed
0.5 s interval (also scaled by the speed selector).

The emit loop releases up to 32 AIS sentences per 1 Hz tick, which is what 60x
on a dense capture needs. That is comfortably inside the link budget: 38400
baud carries 3840 bytes/s, and 32 AIS plus 4 GPS sentences is roughly 1800.

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

## Setting up the toolchain

Everything needed to rebuild the Arduino environment from scratch lives in
`env/`. On a fresh machine, install the [Arduino IDE](https://www.arduino.cc/en/software)
(or standalone `arduino-cli`), then run:

```powershell
.\env\setup_env.ps1 -Verify
```

That installs the pinned core and libraries, puts `lv_conf.h` where LVGL can
find it, applies the board-definition PSRAM fix, and compiles the sketch to
prove the result works. It is safe to re-run — every step is idempotent and
anything it would overwrite is backed up to `.bak` first.

| `env/` file | Purpose |
|---|---|
| `setup_env.ps1` | Installs and configures everything below |
| `lv_conf.h` | LVGL configuration for this panel — **not** obtainable from upstream |
| `boards.local.txt` | The PSRAM board-definition fix |

### What it installs

| Component | Version | Source |
|---|---|---|
| ESP32 core | `esp32:esp32` **3.3.10** | [`package_esp32_index.json`](https://espressif.github.io/arduino-esp32/package_esp32_index.json) |
| `lvgl` | **8.3.3** | Arduino library index |
| `TAMC_GT911` | **1.0.2** | Arduino library index ([source](https://github.com/TAMCTec/gt911-arduino)) |
| `PCA9557-arduino` | **1.0.0** | Arduino library index |

Versions are pinned because these are what the project was built and tested
against. Newer ones may work but are unverified — in particular the ESP32 core
moved to IDF 5.5, which is what broke LovyanGFX for this board.

### Manual setup

If you would rather not run the script:

```sh
# 1. ESP32 core
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.10 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json

# 2. Libraries
arduino-cli lib install "lvgl@8.3.3" "TAMC_GT911@1.0.2" "PCA9557-arduino@1.0.0"
```

3. Copy `env/lv_conf.h` into the sketchbook `libraries/` folder — **beside**
   the `lvgl` folder, not inside it. `lvgl/src/lv_conf_internal.h` includes
   `"../../lv_conf.h"`, which resolves to the libraries root:

   ```
   Documents/Arduino/libraries/
   ├── lv_conf.h        <-- here
   ├── lvgl/
   ├── TAMC_GT911/
   └── PCA9557-arduino/
   ```

   Settings that matter for this panel: `LV_COLOR_DEPTH 16`,
   `LV_COLOR_16_SWAP 0` (the RGB565 framebuffer is not byte-swapped),
   `LV_TICK_CUSTOM 1` driven by `millis()`, and `LV_FONT_UNSCII_8 1` for the
   monospace status line.

4. Copy `env/boards.local.txt` into the core directory, next to `boards.txt`:
   `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.10\`

### Why boards.local.txt is required

The stock `elecrow_crowpanel_7` entry in the ESP32 core's `boards.txt` sets
`psram_type=opi` but **never sets `build.memory_type`**, so it falls back to
`{build.boot}_qspi` and links the *quad* PSRAM driver against this board's
*octal* PSRAM chip. The result:

```
E (116) quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
```

PSRAM then reports 0 MB, and since the 800x480 framebuffer lives in PSRAM the
display never comes up. `boards.local.txt` is an additive override file the
core reads alongside `boards.txt`, so it fixes this without editing a
core-managed file:

```
elecrow_crowpanel_7.build.memory_type={build.boot}_{build.psram_type}
elecrow_crowpanel_7.menu.PSRAM.disabled.build.psram_type=qspi
```

> **This file is deleted whenever the ESP32 core is upgraded or reinstalled.**
> If the display suddenly stops working after a core update, check PSRAM first
> and re-run `env/setup_env.ps1`.

---

## Building

**FQBN**

```
esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled
```

```sh
arduino-cli compile --fqbn esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled .
arduino-cli upload -p COM25 --fqbn esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled .
```

The sketch folder does not need to live in the Arduino sketchbook — the CLI
builds it from any path, and libraries still resolve from the sketchbook.

Footprint: ~42% flash, ~24% RAM.

A harmless notice during the build — `Precompiled library in
".../lvgl/src/esp32s3" not found` — is LVGL's `library.json` declaring itself
precompiled for an architecture that is not present; it falls back to building
from source, which is what we want.

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
| `NMEASimPanel.ino` | LVGL user interface and the 1 Hz emit loop |
| `nmea_sim.h/.cpp` | GPS core: state, motion, sentence builders. **No Arduino dependency** — compiles and runs on a PC |
| `ais_sim.h/.cpp` | AIS core: target table, motion, bit packing, 6-bit armor, `!AIVDM` framing. Also **no Arduino dependency** |
| `ais_play.h/.cpp` | Recorded-log playback with cooperative (non-blocking) pacing. No Arduino dependency |
| `crowpanel_bsp.h/.cpp` | Board support: RGB panel, GT911 touch, backlight |
| `AIS_Recordings/` | Recorded `.ais` captures used by playback |
| `tools/make_ais_log.ps1` | Generates `ais_log.h` from those captures |
| `test/test_nmea.cpp` | Host-side test for the GPS core |
| `test/test_ais.cpp` | Host-side test for the AIS core, with golden vectors |
| `test/test_play.cpp` | Host-side test for playback pacing and log parsing |
| `env/` | Toolchain reconstruction: setup script, `lv_conf.h`, `boards.local.txt` |

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

**The RGB panel needs a bounce buffer, or it tears.** With `fb_in_psram = 1`
and no bounce buffer, the RGB peripheral streams the whole 768 KB framebuffer
out of PSRAM every frame. Anything else writing to PSRAM at the same time — an
LVGL blit, above all a full-pane repaint — starves that DMA, the panel loses
horizontal sync, and the image shifts and stutters sideways. It looks like a
timing or clock fault, and it is not.

```c
cfg.bounce_buffer_size_px = (size_t)BSP_SCREEN_W * 10u;
```

The DMA then reads from small internal-SRAM blocks the driver refills from
PSRAM, decoupling scan-out from PSRAM contention. Two 16 KB buffers against
~177 KB of free internal RAM.

**Redrawing the log pane is expensive.** `lv_textarea_set_text` re-wraps and
re-renders the entire buffer, and the cost is close to linear in its size:
4096 B blocked `lv_timer_handler` for ~400 ms, 1024 B for ~140 ms. The log is
therefore capped at 1 KB and refreshed at most once a second. If you enlarge
it, measure — a 4 KB scrollback makes the UI visibly unresponsive.

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

- Per-target AIS editing in the UI (currently the traffic picture is seeded
  from a fixed table around own ship)
- SD card playback, so captures need not be compiled in (see the pin caveat above)
- More AIS types: 4 (base station), 21 (aid to navigation)
- Additional GPS sentences (GSA, GSV)
- Optional turn rate control in the UI for circular tracks
