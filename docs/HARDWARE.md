# Hardware reality on the reference unit

Everything in this fork has been built and run on **one** device. This document says
exactly what that device is, and — more usefully — where the firmware's own reporting
disagrees with it.

**The rule this page exists to enforce:** ground truth comes from the `i2c` bus scan and
the `free` verb. **Never** from `/systeminfo` `capabilities`.

---

## The unit

An **ESP32-S3-N16R8** module on a **Smoochiee V2 carrier that is only partly populated** —
16 MB flash, 8 MB OPI PSRAM, a 1.47" 172×320 IPS LCD on SPI, five buttons, USB powered.
PlatformIO env `smoochiee-board`.

The `smoochiee-board` profile in this repo describes a **fully populated** Smoochiee V2.
The test unit is not one. Beyond the module, the display and the buttons, nothing is
fitted.

| Profile says | Actually fitted | Confirmed by |
|---|---|---|
| CC1101 (Sub-GHz) | ✗ | visual inspection |
| NRF24 | ✗ | visual inspection |
| PN532 (RFID/NFC) | ✗ | visual inspection |
| GPS | ✗ | visual inspection |
| IR TX/RX | ✗ | visual inspection |
| Buzzer (`-DBUZZ_PIN=44`) | ✗ | visual inspection |
| RGB LED | ✗ | visual inspection |
| Microphone (`-DMIC_INMP441`) | ✗ | visual inspection |
| BQ25896 PMU / charger | ✗ | visual inspection, 2026-07-31 |
| AW9523 I/O expander | ✗ | visual inspection, 2026-07-31 |
| SD card | ✗ (no card) | `SD` totals read `0 B` |

The whole I²C bus is empty as a result: `i2c` answers **`No I2C devices found`** (measured
2026-07-31). That is correct behaviour for this unit, not a bug — it was investigated as
one first.

---

## Where the firmware misreports it

### `capabilities` are compile-time `#define` checks, not runtime probes

`/systeminfo` reports `has_cc1101`, `has_nrf24`, `has_gps`, `has_ir`, `has_buzz`,
`has_rgb_led` and `has_mic` all **`true`** on a board with none of them
([ISSUE-4](./KNOWN_ISSUES.md)). They describe the board *profile*.

**Never gate behaviour on them.** An app that greys out buttons from `capabilities` will
offer every RF, IR and NFC feature on a device that cannot do any of them.

### `battery_pct` is permanently `1`, `charging` permanently `false`

No PMU is fitted. `getBattery()` and `isCharging()` early-return on a stored `pmu_present`
flag (`boards/smoochiee-board/interface.cpp:19,45,64,74`), which is what removed the old
`ESP_ERR_INVALID_STATE` console storm — but the returned value is still a lie
([ISSUE-3](./KNOWN_ISSUES.md), partially fixed in `b1c825c8`).

A sentinel was rejected: the value is assigned to a `uint8_t`, so `-1` renders as 255 %. An
honest "no battery" needs a cross-board `batteryPresent()`, which does not exist yet.

### `rf selftest` does not exist in this build

Earlier notes recommended it as CC1101 ground truth. The device answers
`ERROR: Command not found at 'rf selftest'` (verified 2026-07-29): its registration sits
inside `#if RF_DEBUG` (`src/modules/rf/rf_commands.cpp`), and `RF_DEBUG` defaults to `0`
(`src/modules/rf/protocols/rf_config.h:19-21`) and is not overridden for this board. The
same guard hides `keeloqtest` and `keeloqfiletest`.

### Storage

No SD card, so `SD` totals read `0 B`. Everything lives on LittleFS — 11.4 MB
(`custom_16Mb.csv`).

---

## The two USB ports

The board has **two** USB connectors and they are not interchangeable. Getting this wrong
once cost a day and produced a "soft-bricked board" that was never bricked.

| Connector | Enumerates as | Carries | Feeds the panel? |
|---|---|---|---|
| CH343 UART bridge | `1a86:55d3` (typically `/dev/ttyACM0`) | `ESP_LOG`, panic backtraces, `RAM_LOG` — **the console** | **Yes** |
| Native USB (ESP32-S3's own) | `303a:1001` (typically `/dev/ttyACM1`) | Arduino `Serial` / USB-CDC | No |

Measured 2026-07-31, both directions, with `lsusb` watching both ports:

| Cable pulled | Panel | Board |
|---|---|---|
| CH343 | **DARK** | still running, no reset |
| Native USB | **LIT** | still running, no reset |

The schematic (`pcbs/Bruce_PCB_smoochie/Bruce_PCB_v2_Schematic.pdf`) shows a **single**
USB-C input, `USB1 / TYPE C SMD 16PINS`, feeding the charger block — that is the CH343
side. The native USB port powers only the module's own VDD33 and does not backfeed the
carrier rail. (The exact rail path is not fully established, because the charger IC in that
block is not populated on this unit. The measurement is what stands.)

**Practical rule: plug in both cables.** CH343 for panel power and the log console, native
USB for the CDC port.

> **The "soft-brick" that wasn't.** With only the native cable in, the ESP32 boots normally
> and answers over USB-CDC, but the panel is dead — which looks exactly like a bricked
> board. It is not. It is a hardware port asymmetry, not a firmware fault, and not a PMU
> fault: the panel goes dark whether the PMU init succeeds or fails, as long as CH343 is
> out. `TFT_BL` (IO 6) is not involved either — pulling CH343 never touches it.

---

## Pins worth knowing

| Pin | Role | Source |
|---|---|---|
| 47 / 48 | `SDA` / `SCL` — the I²C bus | `boards/smoochiee-board/pins_arduino.h:10-11` |
| 47 / 48 | *also* `GROVE_SDA` / `GROVE_SCL` | `pins_arduino.h:88-89` |
| 43 | UART0 TX — where `RAM_LOG` is mirrored | `src/core/ram_profile.cpp:20` |
| 44 | UART0 RX; also `-DBUZZ_PIN=44` in the board `.ini` | `smoochiee-board.ini` |
| 6 | `TFT_BL` | `pins_arduino.h:70` |

**Consequence for the `gpio` verb**: the only two pins it can usefully drive on this board
*are* the I²C bus lines. That is a code fact from the pin map, not a measurement — an
attempt to demonstrate bus corruption through it was **not** conclusive and the causation
claim was withdrawn. Still: `gpio` is not a verb to expose as a one-tap app action here.

---

## Other boards

**No claim is made about any of them.** Upstream Bruce supports a long list of M5Stack,
Lilygo and other devices; those environments are still in this tree and untouched, but
nothing in this fork has ever been compiled or run for one.

The radio-coexistence work in particular is tuned to the memory profile of this exact chip.
Boards with less internal DRAM, or without PSRAM, may behave differently or not work at
all. See [ROADMAP.md](./ROADMAP.md) for what would have to happen to change that.
