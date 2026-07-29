# Board Evaluation — Waveshare ESP32-S3-Touch-LCD-7

**Date:** 2026-07-29
**Evaluated for:** NMEASimPanel — candidate replacement for the Elecrow CrowPanel Advance 7.0 (WZ8048C070)
**Driving requirement:** two independent USB/COM ports on one device, so a PC can consume a GPS stream and an AIS stream separately
**Verdict:** Meets the requirement. Verified at schematic netlist level. Recommended only if the two-port requirement is held firm — see *Recommendation*.

> **Status update — the board was not bought, and the requirement dissolved.**
>
> The recommendation below was to build `ais_sim` first and buy hardware only if
> the single-link approach failed. That was done, and it did not fail:
> NMEASimPanel now emits GPS and AIS interleaved on one 38400-baud link, plus
> recorded-capture playback, all on the existing CrowPanel. Nothing has needed a
> second COM port.
>
> This document is kept as the record of *why* the second port was not needed and
> what the alternative would have cost. The schematic analysis in sections 4–5
> remains accurate for the Waveshare V1.2 board should the question return —
> particularly the GPIO19/20 finding, which is the whole basis of the comparison.
>
> One caveat that aged: section 7 assumed the AIS work was still ahead. It is
> done, and the pure-logic layer did port as predicted — `ais_sim` and `ais_play`
> have no Arduino dependency and are host-tested.

---

## 1. Why this board was evaluated

The current CrowPanel presents exactly one COM port. That is not an ESP32 limitation but a board layout consequence:

Every ESP32-S3 has two independent paths to a host PC — the native USB peripheral on GPIO19/20, and a UART bridge chip. The CrowPanel spends GPIO19/20 on the GT911 touch I2C bus, so the native USB path is physically unavailable. Its single USB-C connector goes to a CH340 bridge. There is no firmware fix; the pins are consumed.

The search criterion was therefore narrow and checkable: **a 7" ESP32-S3 panel that keeps GPIO19/20 free.** The Waveshare ESP32-S3-Touch-LCD-7 does.

## 2. Sources and verification method

| Source | Used for | Reliability |
|---|---|---|
| `ESP32-S3-Touch-LCD-7-Sch.pdf` (V1.2), files.waveshare.com | Netlist verification — primary evidence | Authoritative |
| docs.waveshare.com/ESP32-S3-Touch-LCD-7 | Feature list, resource index | Vendor doc |
| Community ESPHome config (github.com/inytar) | Independent corroboration of touch pins | Secondary |
| Bot'n Roll listing (SKU 27078) | SKU confirmation, price anchor | Retail |

Netlist facts below were extracted from the schematic PDF's text layer, not read off a marketing table. Net names are quoted verbatim.

**Scope limit:** verification applies to board revision **V1.2**. Earlier or later revisions are not covered.

## 3. Hardware summary

| Item | Detail |
|---|---|
| MCU | ESP32-S3-WROOM-1 |
| Display | 800×480 RGB565, ST7262 |
| Touch | GT911 capacitive, I2C |
| IO expander | **CH422G** (CrowPanel uses PCA9557) |
| USB bridge | **CH343P** |
| USB mux | **FSUSB42UMX** ×2 (U9, U13) |
| CAN transceiver | TJA1051 |
| Backlight driver | AP3032 boost |
| Connectors | 2× USB-C, RS485, CAN, I2C, UART2 header, microSD, PH2.0 battery |
| Battery | 3.7V single-cell Li-ion with charge circuit, CHG/DONE LEDs |

## 4. The dual-USB architecture (verified)

This is the finding the whole evaluation rests on.

### Port A — `Type_C2`, silkscreen "USB" — native USB

```
ESP32-S3 GPIO19 -> ESP_USB_N ─┐
ESP32-S3 GPIO20 -> ESP_USB_P ─┴─> U9 FSUSB42UMX (2:1 USB mux)
                                    ├─ HSD1 -> ESP_USBH_N/P -> R65/R66 (0R) -> USBH_N/P -> Type_C2 D-/D+
                                    └─ HSD2 -> CANRX / CANTX -> TJA1051
                                  SEL (pin 10) <- USB_SEL <- CH422G EXIO5
```

GPIO19/20 are muxed between the USB-C connector and the CAN transceiver. Selecting USB mode (EXIO5) makes CAN unavailable — an acceptable trade for this project, which does not use CAN.

### Port B — `Type_C1`, silkscreen "UART1" — CH343P bridge

```
Type_C1 D-/D+ -> USBD_N/P -> R5/R10 (0R) -> USB_N/P -> U1 CH343P (pins 8/7, UD-/UD+)
CH343P TXD/RXD -> CH_TXD / CH_RXD -> U13 FSUSB42UMX
                                       ├─ HSD1 -> CH343P        (SW1 position "UART1")
                                       └─ HSD2 -> EX_TXD/EX_RXD -> H3 header (SW1 position "UART2")
                                     common -> ESP_TXD / ESP_RXD = GPIO43 / GPIO44 (UART0)
                                     SEL <- UART_SEL <- SW1 (10K pull-up R17)
```

**The two paths are fully independent silicon.** Native USB peripheral vs CH343P on UART0. Both enumerate simultaneously. Requirement met.

### Two non-obvious findings

**The "UART1/UART2" switch is not two ESP32 UARTs.** SW1 selects where **UART0** is routed — to the CH343P bridge, or to the external H3 header (3V3/GND/RXD/TXD). One UART, two destinations. For this project SW1 stays in the UART1/CH343P position.

**Only the bridge port has an auto-reset circuit.** CH343P DTR/RTS drive SS8050 transistors (Q6/Q9) into `RESET` and `BOOT`. `Type_C2` is a direct native-USB connection with no such circuit. The reset-on-connect hazard documented in `README.md:279` therefore applies to one port only, and the native port is immune to it.

## 5. GPIO map (from schematic pin legend)

| GPIO | Net | Function |
|---|---|---|
| IO0 | G3 | LCD green |
| IO1 | R3 | LCD red |
| IO2 | R4 | LCD red |
| IO3 | VSYNC | LCD sync |
| IO4 | CTP_IRQ | Touch interrupt |
| IO5 | DE | LCD data enable |
| IO6 | AD | Sensor ADC |
| IO7 | PCLK | LCD pixel clock |
| **IO8** | **SDA** | **Touch I2C data** |
| **IO9** | **SCL** | **Touch I2C clock** |
| IO10 | B7 | LCD blue |
| IO11 | MOSI | microSD |
| IO12 | SCK | microSD |
| IO13 | MISO | microSD |
| IO14 | B3 | LCD blue |
| IO15 | RS485_TX / RTCN | RS485 |
| IO16 | RS485_RX / RTCP | RS485 |
| IO17 | B6 | LCD blue |
| IO18 | B5 | LCD blue |
| **IO19** | **ESP_USB_N** / CANRX | **Native USB D− (muxed w/ CAN)** |
| **IO20** | **ESP_USB_P** / CANTX | **Native USB D+ (muxed w/ CAN)** |
| IO21 | G7 | LCD green |
| IO38 | B4 | LCD blue |
| IO39 | G2 | LCD green |
| IO40 | R7 | LCD red |
| IO41 | R6 | LCD red |
| IO42 | R5 | LCD red |
| **IO43** | **ESP_TXD** | **UART0 TX → CH343P** |
| **IO44** | **ESP_RXD** | **UART0 RX → CH343P** |
| IO45 | G4 | LCD green |
| IO46 | HSYNC | LCD sync |
| IO47 | G6 | LCD green |
| IO48 | G5 | LCD green |

### CH422G expander map

| Pin | Net | Function |
|---|---|---|
| EXIO1 | CTP_RST | GT911 reset |
| EXIO2 | DISP | Panel DISP **and** backlight enable (→ `BL_EN_1` → AP3032 EN) |
| EXIO3 | LCD_RST | Panel reset |
| EXIO4 | SDCS | microSD chip select |
| EXIO5 | USB_SEL | U9 mux: USB vs CAN |
| EXIO6 | LCD_VDD_EN | Panel supply enable |

## 6. Comparison with the current CrowPanel

| | CrowPanel Advance 7.0 | ESP32-S3-Touch-LCD-7 |
|---|---|---|
| Panel | 800×480 RGB565 | 800×480 RGB565 (ST7262) |
| Touch | GT911 @ 0x5D, **GPIO19/20** | GT911, **GPIO8/9** |
| IO expander | PCA9557 @ 0x18 | CH422G |
| USB ports | 1 (CH340) | **2 (native + CH343P)** |
| COM ports to PC | 1 | **2** |
| Native USB usable | No — pins consumed by touch | Yes |
| Backlight | GPIO2, LEDC PWM | EXIO2 enable via AP3032 |
| Extras | — | RS485, CAN, microSD, battery + charger |

## 7. Impact on NMEASimPanel if ported

**Unaffected.** The pure-logic layer moves untouched by design — `nmea_sim`, `ais_sim`, `ais_play` and the PC-side unit tests in `test/` have no Arduino or board dependencies. This is the payoff of the existing separation. (Written when only `nmea_sim` existed; the prediction held for the two modules added since.)

**Requires rework — confined to `crowpanel_bsp.cpp`:**

1. **IO expander: PCA9557 → CH422G.** Different part, different register interface. The GT911 bring-up sequence documented at `crowpanel_bsp.cpp:102-137` must be re-derived for the new expander. Given the effort that sequence originally took, budget conservatively.
2. **Touch I2C moves** from GPIO19/20 to GPIO8/9.
3. **Backlight control changes character.** CrowPanel drives GPIO2 with LEDC PWM for smooth dimming. This board gates an AP3032 boost driver via expander pin EXIO2. Software PWM over I2C would be slow and coarse — **assume on/off only until proven otherwise**, and verify against the Waveshare demo code if dimming matters.
4. **Panel reset and supply enable** move to EXIO3 / EXIO6.
5. **New init step:** drive EXIO5 low to select USB mode, or the native USB port will not enumerate.
6. **Serial mapping inverts.** On the CrowPanel, `Serial` (USB-CDC) goes nowhere and everything uses `Serial0`. Here both are live:
   - `Serial` (USB-CDC, GPIO19/20) → `Type_C2` → COM port 1
   - `Serial0` (UART0, GPIO43/44) → CH343P → `Type_C1` → COM port 2

   Two streams, no TinyUSB composite-CDC work required.

**Estimate:** one to two days of bring-up, dominated by re-fighting GT911 initialisation on an unfamiliar expander.

## 8. Risks and open items

| Item | Severity | Notes |
|---|---|---|
| Backlight dimming may be lost | Medium | EXIO2 is an enable line, not a PWM GPIO. Unverified whether the demo achieves dimming. |
| GT911 bring-up on CH422G | Medium | Known-hard on this panel class. Prior experience transfers conceptually, not literally. |
| Board revision drift | Medium | Verification covers **V1.2** only. Confirm silkscreen on arrival. |
| Amazon listing identity | Medium | Listings get relabelled. Confirm two USB-C ports in photos before ordering. |
| CAN unavailable in USB mode | None | Not used by this project. |
| Boot log on UART0 | Low | ROM bootloader prints at 115200 on `Type_C1` regardless of selected baud — same cosmetic issue as `README.md:284`. `Type_C2` is unaffected. |

## 9. Sourcing

Waveshare SKU: **27078**. Search term: `ESP32-S3-Touch-LCD-7` or `Waveshare 27078`.

| Source | Detail | Status |
|---|---|---|
| Amazon.ca `B0D3DVX6CT` | Attributed to Waveshare as seller | Likely correct — verify photos |
| Amazon.ca `B0D3LTBQBP`, `B0DRJH2X91` | Alternate ASINs, same apparent product | Unverified |
| Bot'n Roll (PT) | €44.90, SKU 27078; description confirms CAN/RS485/battery | Confirmed listing |
| waveshare.com/esp32-s3-touch-lcd-7.htm | Vendor direct | Valid page, price unread |
| rarecomponents.com | URL exists | Country and price unverified |
| DigiKey Canada | **Zero results** for part number | Confirmed absent |
| Mouser.ca, RobotShop | Request blocked/timed out | Unverified |

Expected landed cost roughly CAD $70–90 based on the €44.90 anchor.

## 10. Recommendation

**The board is technically confirmed and would work.** If the two-port requirement is firm, buy it.

**However, the requirement itself is worth re-examining before spending money.** Real AIS transponders emit GPS and AIS on a single NMEA 0183 link at 38400 baud — `$GPRMC` and `!AIVDM` interleaved on one wire is the standard, expected format, not a compromise. Chart plotters and PC applications demux by talker ID as a matter of course. Splitting the existing single port PC-side costs nothing:

- **Both apps accept UDP/TCP:** a small bridge reads the COM port once and broadcasts to `255.255.255.255:10110`. Unlimited consumers.
- **An app is serial-only:** com0com + hub4com, or a demux on the leading `$` vs `!` character into two virtual COM ports.

Either approach delivers two independent streams to two applications on the existing hardware, today.

**The gating work is the same either way:** `ais_sim` does not exist yet *(at time of writing)*. AIS does not fit the current sentence registry — it needs a multi-target state table, 6-bit-ASCII payload armouring, multi-fragment sentences for type 5, and a non-1 Hz emission schedule. That is the critical path regardless of which board it runs on.

**Suggested order:** build `ais_sim` on the CrowPanel, prove the mixed stream against the actual consuming applications, and buy new hardware only if that demonstrably fails.

> **Outcome.** This order was followed. `ais_sim` was built by porting the
> validated WireCodecs `ais` encoder to fixed buffers, and `ais_play` was added
> for recorded-capture replay. Both run on the existing CrowPanel over the single
> link. The board was not purchased.
>
> The one step still outstanding is the one this recommendation hinged on:
> **proving the mixed stream against a real chart plotter.** Everything so far is
> verified at the byte level — against the reference encoder, against the source
> captures, and on the wire — but no actual consumer has demuxed it.

---

## References

- Schematic: `ESP32-S3-Touch-LCD-7-Sch.pdf` V1.2 — files.waveshare.com/wiki/ESP32-S3-Touch-LCD-7/
- Wiki: docs.waveshare.com/ESP32-S3-Touch-LCD-7
- Datasheets: CH343 (CH343DS1), CH422G (CH422DS1), GT911, ST7262, TJA1051
- Current board notes: `README.md`, `crowpanel_bsp.cpp`
