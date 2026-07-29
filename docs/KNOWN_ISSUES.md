# Known issues — verified defects

A defect register for this fork. Companion to
[bruce-companion-api.md](./bruce-companion-api.md) (what the interface is) and
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) (why the firmware was changed).

**Rules for this file.** One entry per defect. Every entry carries the evidence that
established it — a decoded backtrace, a console capture, a measurement with a date —
or it does not belong here. Distinguish **VERIFIED** (reproduced, with evidence
attached) from **SUSPECTED** (inferred from code, never observed). When something is
fixed, move it to §Resolved with the commit that fixed it and the test that proves
it. Do not delete entries; a fixed bug that regresses is easier to recognise if its
history is still here.

**Hardware under test:** bare ESP32‑S3‑N16R8 devkit, 1.47" 172×320 IPS LCD (ST7789)
on SPI, five buttons on GPIO 0/38/39/40/41, USB‑powered. No PMU, no SD card, no
CC1101/NRF24/PN532/IR/GPS. Built as env `smoochiee-board`.

---

## Open

### ISSUE-1 — `deauth` crashes the device (FreeRTOS assert)

**Status:** OPEN · **Severity:** critical · **Verified** 2026-07-29 · **Reproduced 2/2**

Sending `deauth` over the BLE CLI does not merely block the serial task, as §5.2 of
the API contract described until now. It **panics the device and reboots it**,
within ~20–70 s, with no further user action.

**Panic**, captured on the USB CDC console:

```
assert failed: xTaskPriorityDisinherit tasks.c:5156 (pxTCB == pxCurrentTCBs[ xPortGetCoreID() ])
Backtrace: 0x40384c39:0x3fcd70a0 0x40384c01:0x3fcd70c0 0x4038c042:0x3fcd70e0 ...
ELF file SHA256: 2841bf2b5
Rebooting...
rst:0xc (RTC_SW_CPU_RST)
```

The reported ELF SHA256 matches `.pio/build/smoochiee-board/firmware.elf`
(`2841bf2b5d31`), so the decode below is authoritative rather than approximate.

**Decoded backtrace** (`xtensa-esp32s3-elf-addr2line`), innermost last:

```
_serialCmdsTaskLoop          serialcmds.cpp:84         <- the serial task
handleSerialCommands         serialcmds.cpp:66         <- parse() runs inline
SimpleCLI::parse
deauthCmdCallback            attack_commands.cpp:153   <- fork code
wifi_atk_menu                wifi_atks.cpp:358
loopOptions                  display.cpp:704
tft_logger::drawArc          tftLogger.cpp:417
TFT_eSPI::drawArc            TFT_eSPI.cpp:4252
  -> end_tft_write           TFT_eSPI.cpp:114
SPIClass::endTransaction     SPI.cpp:214
spiEndTransaction            esp32-hal-spi.c:1340
xQueueGenericSend -> prvCopyDataToQueue -> xTaskPriorityDisinherit -> assert
```

**Root cause — cross-task SPI mutex release.** `spiEndTransaction()` returns the SPI
bus mutex. The assertion `pxTCB == pxCurrentTCBs[coreID]` fires when the task
*releasing* a mutex is not the task that *took* it. The menu-dispatcher verbs run
TFT drawing code on the **serialcmds task**, while the Arduino main loop task is
concurrently drawing the main menu and status bar through the *same* `TFT_eSPI`
instance on the *same* SPI bus. Nothing arbitrates between them.

A likely trigger cadence: the main-menu path redraws on a timer —
`if (millis() - _clock_bat_timer > 30000) drawStatusBar();` (`display.cpp`) — so a
collision becomes near-certain within ~30 s of launching such a verb. Observed crash
latencies of ~20 s and (first run) somewhere in 70–130 s are consistent with that.

**Why this is fork-introduced, not upstream.** Upstream reaches these menus only from
the main loop task, driven by physical navigation, so only one task ever touches the
display. `attack_commands.cpp` — a new file in this fork — invokes the same menu code
from the serialcmds task. Upstream's own serial verbs (`ls`, `cat`, …) never open a
TFT menu, so upstream never reaches this path.

**Scope.**

Each verb was dispatched over BLE and left for a 90 s window with the USB console
captured throughout; "no crash" means no `assert failed` / `Backtrace:` /
`Guru Meditation` / `rst:0x` appeared in that window. **A 90 s clean window is not a
proof of safety** — `deauth` itself took 70–130 s to fall over on its first run.

| Verb | Path | Blocks? | Crash? | Telemetry |
|---|---|---|---|---|
| `deauth` | `wifi_atk_menu()` | yes | ☠️ **CRASHES** — 2/2, backtrace above | none |
| `karma` | `karma_setup()` | yes | no crash in 90 s | none |
| `blesniffer` | `BleSuiteMenu()` | yes | no crash in 90 s | none |
| `ap_info` | `displayAPInfo()` | yes (>4 min) | no crash | none |
| `pwngrid` | `brucegotchi_start()` | yes | no crash in 90 s | none |
| `reverseshell` | `ReverseShell()` | **no** — returned in 90 ms | no crash | none, and a false `TRUE` (ISSUE-7) |
| `evilportal` | `EvilPortal::loop()` | yes | no crash in 100 s | ✅ `state: portal` |

Notes from the sweep:

- `blesniffer` did **not** drop the BLE control link — opening the BLE Suite menu does
  not tear down NimBLE by itself, contrary to what the v1.x conflict notes implied.
- `ap_info`'s exit is SELECT-only; Esc is ignored entirely. See ISSUE-6.
- **`evilportal` is the only one of the seven that emits usable telemetry**, and the
  only one where the app can confirm the attack actually started rather than merely
  that the callback was entered. Frame observed at +3.51 s:
  `{"id":4,"type":"state","device_state":"portal"}`. This reproduces maritest's
  2026-07-27 observation.

**Only `deauth` crashes.** That is a narrower blast radius than feared, and the shape
of the exception is informative. `wifi_atk_menu`'s `loopOptions` path drives `drawArc`
**continuously**. Every verb that survived redraws only on an event:
`ScrollableTextArea` redraws on input (`ap_info`), and `EvilPortal::loop()` calls
`drawScreen()` only when `shouldRedraw` is set — on a credential capture, a WiFi
restart, or a button press (`evil_portal.cpp:290-320`).

So the working hypothesis is that the collision needs *sustained* drawing from the
serial task, not merely *any* drawing. Two consequences, both unproven:

- A quiet UI is not a safe UI, it is an unlikely-to-collide one. A clean 90 s window
  is **not** proof — `deauth` itself survived 70+ s on its first run before dying.
- **`evilportal` was tested idle**, with nothing associating to the portal. Under real
  load — phones connecting, credentials landing — `shouldRedraw` fires repeatedly and
  its drawing pattern moves toward `deauth`'s. That case has **not** been tested and
  is the obvious next experiment.

**Consequences for the companion app.** These verbs cannot be shipped as one-tap
actions. The failure is worse than the documented one: the app does not merely lose
the ability to end the verb remotely, it loses the device. Note the crash also
destroys any state the operator had built up, and `bleApiAutoStart` brings the BLE
API back on the next boot, so the app will see a reconnect rather than an error —
which reads as a transport glitch, not a firmware crash. An app that retries on
reconnect would crash-loop the device.

**Repro.**

```sh
# terminal 1 — capture the panic; the console is the only place it appears
python3 tools/…/usb_console_capture.py     # or: cat /dev/ttyACM0 at 115200 raw
# terminal 2 — dispatch over BLE and wait
python3 tools/ble_spike/probe_verbs.py     # then write "deauth\n" to d555ed97-…
```

Decode with:

```sh
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/smoochiee-board/firmware.elf <backtrace addresses>
```

**Fix direction (not implemented).** Either serialise all TFT access behind a single
owner — a display mutex taken by both tasks, or marshalling draw calls onto the loop
task — or stop drawing from the serial task at all by giving these verbs headless
entry points that take their parameters as arguments (the approach `blespam` already
uses successfully). The second also fixes the "requires on-device interaction"
limitation, so it is the better target.

---

### ISSUE-2 — `settings` with no arguments returns nothing over BLE

**Status:** OPEN · **Severity:** low · **Verified** 2026-07-29

`settings` with no arguments is documented as "View all the current settings", but
over BLE it returns 5 bytes — `\r\n` plus the `# ` prompt plus EOT — and no JSON.

Cause: `settingsCallback` calls `serializeJsonPretty(jsonDoc, Serial)`
(`src/core/serial_commands/settings_commands.cpp:19`) — the `Serial` object, not
`*serialDevice`. When the BLE API is armed, `serialDevice` points at the GATT service
(`ble_api.cpp:63`), so the config never reaches the client. A correlated 22 s USB CDC
capture during the command did not show the JSON on that port either.

Single-field reads are unaffected: `settings bright` → `bright = 100`.

**Fix:** `Serial` → `*serialDevice` in that one call.

---

### ISSUE-3 — `battery_pct` and `charging` are fabricated with no PMU fitted

**Status:** OPEN · **Severity:** medium (blocks any battery UI) · **Verified** 2026-07-29

`getBattery()` and `isCharging()` call `PPM.*` unconditionally, ignoring the
`pmu_ret` result of `PPM.init()` (`boards/smoochiee-board/interface.cpp:38-75`). With
no BQ25896 on the bus the reads fail, voltage returns 0, the percentage computes to
roughly −412, and `if (percent < 0) return 1` clamps it.

Measured: `/systeminfo` reports **`"battery_pct":1, "charging":true` permanently**.
The failing polls also emit a continuous
`i2cWrite(): i2c_master_transmit failed: [259] ESP_ERR_INVALID_STATE` stream on the
console, roughly 10 per 22 s, indefinitely.

A battery is planned for this board later, so this becomes a live correctness bug
rather than a cosmetic one once fitted — the guard should be added regardless.

**Fix:** store `pmu_ret` and return a sentinel (or omit the fields) when the PMU is
absent.

---

### ISSUE-4 — `/systeminfo` capability flags describe the board profile, not the board

**Status:** OPEN · **Severity:** medium · **Verified** 2026-07-29

The `capabilities` object is built from compile-time `#if defined(...)` checks against
`boards/smoochiee-board/pins_arduino.h`, which describes a fully populated Smoochiee
V2. On the bare devkit under test it reports `has_cc1101`, `has_nrf24`, `has_gps`,
`has_ir`, `has_buzz`, `has_rgb_led` and `has_mic` all `true`, while `i2c` on the same
device returns `No I2C devices found`.

This matters more as modules get added over time: an app cannot distinguish "fitted"
from "in the board profile", so it cannot adapt as hardware arrives.

**Fix:** back the flags with runtime probes — `ELECHOUSE_cc1101.getCC1101()` already
exists and is used by `initRfModule`, an I2C scan covers the expander and PMU, and SPI
presence covers NRF24. Until then, consumers must keep their own truth table; the
maritest app does exactly this in `src/flows/hardware.ts`.

---

### ISSUE-5 — `deauth <target>` argument is accepted and discarded

**Status:** OPEN · **Severity:** low · **Verified** by code

`deauthCmdCallback` ignores its `target` argument entirely and calls `wifi_atk_menu()`
(`attack_commands.cpp:152-155`). The CLI advertises a parameter that has no effect.

Superseded in practice by ISSUE-1 — the verb cannot be used at all right now.

---

### ISSUE-6 — blocking verbs need *different* buttons to dismiss, with no way to know which

**Status:** OPEN · **Severity:** medium (operational) · **Verified** 2026-07-29

There is no single "get me out of this" button. The exit condition depends on which
UI primitive a verb happens to use:

**First, there is no Esc button on this board.** `InputHandler()`
(`boards/smoochiee-board/interface.cpp:119-123`) synthesises Esc from a **chord**:

```c
if (!_s) { SelPress = true; }
if (!_l && !_r) {          // LEFT (GPIO 39) + RIGHT (GPIO 38) held together
    EscPress = true;
    NextPress = false;
    PrevPress = false;
}
```

So every instruction of the form "press Esc" is unactionable as written — the
operator must know to press two buttons simultaneously. This is not discoverable
from the UI.

| Verb | UI primitive | Exit requires |
|---|---|---|
| `deauth`, `karma`, `blesniffer` | `loopOptions()` | **Esc chord (LEFT+RIGHT)** (`display.cpp:647`) *or* selecting the `Main Menu` entry that `addOptionToMainMenu()` appends (`utils.cpp:27-30`) |
| `ap_info` | `ScrollableTextArea::show()` | **SelPress (SELECT/OK) only** — `while (!check(SelPress))` (`scrollableTextArea.cpp:81`). Up/Down scroll; the Esc chord is ignored entirely. |
| `evilportal` | custom `while(true)` | **Esc chord**, then choose `Exit Portal` from the submenu (`evil_portal.cpp:326-363`) |

Observed the hard way, twice: repeated single-button presses did nothing to an
`ap_info` session that stayed blocked for over four minutes (that screen only listens
for SELECT), and a `deauth` menu was never dismissed at all because the operator
reasonably reported having no Esc button — which is literally true.

**App impact.** The companion app cannot tell an operator which button to press,
because the answer depends on firmware internals the app has no visibility into. Any
"Requires on-device interaction" badge is therefore incomplete advice. This
disappears if the verbs get headless entry points; until then the app should name the
verb and let the operator work it out, rather than instruct a specific button.

---

### ISSUE-7 — `[CLI] Result: TRUE` is hardcoded for every attack verb

**Status:** OPEN · **Severity:** high (app cannot detect failure) · **Verified** 2026-07-29

All six attack callbacks discard the outcome and return `true` unconditionally
(`attack_commands.cpp:147-175`):

```cpp
uint32_t reverseshellCmdCallback(cmd *c) {
    ReverseShell();     // return value, if any, is discarded
    return true;
}
```

Identical shape for `karma`, `deauth`, `blesniffer`, `ap_info` and `pwngrid`.
`evilportalCmdCallback` likewise ends in a bare `return true`. Only `blespam`
returns a meaningful result, and only for the argument-validation path.

**Caught live.** `reverseshell` was dispatched with no usable WiFi config. The console
showed the AP creation failing outright:

```
+4.45s  USB    [E][AP.cpp:225] create(): passphrase too short!
+4.48s  EVENT  {"id":4,"type":"log","line":"[CLI] Result: TRUE","level":"info"}
```

The device reported success 30 ms after the operation failed.

**App impact.** `[CLI] Result:` is a **completion** signal, not a **success** signal.
It answers "did the callback return", which for these verbs is always yes. An app
that renders `Result: TRUE` as a green tick will report a successful attack that never
started. There is currently **no** in-band way to learn that one of these verbs
failed — the only evidence was on the USB console, which the app cannot see.
`ble_progress`/`ble_result` frames exist for the BLE spam path and are the only real
outcome telemetry in the firmware.

**Fix:** propagate the real return value where the underlying function has one, and
emit a `ble_result`-style outcome frame where it does not.

---

### ISSUE-10 — `blespam samsung` transmits Galaxy Buds packets with no Flags AD structure

**Status:** OPEN · **Severity:** low · **Verified** 2026-07-29 · **Pre-existing,
unrelated to `c9c43c03`** · **Console capture, 30/30 packets**

The Galaxy Buds payload fills the entire advert budget on its own, and the flags the
caller then adds do not fit, so they are silently discarded on every packet.

`Buds_Data[31]` is filled to all 31 bytes and passed to `AdvData.addData(Buds_Data, bi)`,
after which the shared tail of the case does `AdvData.setFlags(0x06)` — a 3-byte AD
structure that takes the total to 34. NimBLE rejects it and logs. Captured on
`/dev/ttyACM0` during `blespam samsung 30`:

```
E NimBLEAdvertisementData: Data length exceeded      x30
```

Exactly one per packet, because the CLI path pins `device_index = 0` (Galaxy Buds) so
every packet takes the buds branch. The 31-byte payload itself still transmits — the
earlier packet capture did see Samsung company-ID 117 adverts — so this is a
malformed-advert issue, not a no-op like ISSUE-8 was.

**Not caused by the ISSUE-8/9 fix**: `c9c43c03` touches teardown and the MAC snapshot,
not the per-packet build. **Not verified** whether the missing flags actually affect
whether a handset shows the popup.

---

### ISSUE-11 — one unexplained reboot during a back-to-back spam sweep, not reproduced

**Status:** OPEN · **Severity:** unknown · **Observed once** 2026-07-29 · **No console
capture of the event** · **Not reproduced in 2 subsequent attempts**

During the first post-fix verification sweep the device rebooted once. It was detected
after the fact, not observed: `uptime` read `00:01:00` with `free` reporting
`t=60638ms`, when at least ~300 s of testing had run since the flash. Every later
reading was monotonic, so the reset happened during that sweep and not since.

**No USB console was being captured at the time**, so there is no backtrace and no
reset reason. Two deliberate reproduction attempts with `/dev/ttyACM0` captured
throughout — a five-verb sweep, and a FastPair run followed by a five-verb sweep —
both ran clean: `millis()` monotonic across every sample (150s->592s over 46 samples,
431s->592s over 19) and no `rst:0x` / `Backtrace:` / `assert failed` / `ESP-ROM`
markers in either capture. That is ~441 s of continuous uptime spanning both sweeps,
against a fault that showed up inside ~300 s the one time it occurred.

**Do not read this as fixed or as caused by `c9c43c03`; neither is established.** It
is recorded because a one-off reset that is not understood is worth recognising if it
recurs. Next step if it does: keep `usbwatch2.py` running for the whole session so the
event is captured rather than inferred.

---

## Cosmetic / upstream

Recorded because they appear on every boot and are easy to mistake for real faults
when reading a console capture.

- `[E][esp32-hal-gpio.c:185] __digitalWrite(): IO 6 is not set as GPIO. Execute digitalMode(6, OUTPUT) first.`
  — TFT backlight pin, once per boot. Harmless.
- `[E][sd_diskio.cpp:761] sdcard_mount(): f_mount failed: (3)` ×2 — no SD card fitted.
- `[E][vfs_api.cpp:33] open(): does not start with /` then
  `THEME: Theme file not found. Using default theme` — no theme file on LittleFS.
- `[E][STA.cpp:540] disconnect(): STA disconnect failed! 0x3001: ESP_ERR_WIFI_NOT_INIT`
  — emitted by the `wifi_atk_unsetWifi()` path when WiFi was never initialised.
- `webui -bg` prints `Press ESC to quit` even though it returns immediately. Stale
  text inherited from the foreground path; the command does return.

---

## Verified working

Recorded so nobody "fixes" these, and so a regression is recognisable. All measured
on the hardware above, 2026-07-29, over the BLE CLI characteristic unless stated.

| Behaviour | Evidence |
|---|---|
| BLE API re-arms itself at boot | Boot log: `[BLE_API] setup: adv start=1 isAdvertising=1` at `t=8593ms`, after `before-wifi-init` at `t=999ms`. `settings bleApiAutoStart` → `1`. Survived an unplanned crash-reboot. |
| BLE API memory cost ~62 KB | RAMLOG across the transition: `ble-api pre-setup` heap 145,503 / largest 69,620 → `post-setup` heap 81,703 / largest 31,732. |
| Dispatch ACK for blocking verbs | `COMMAND: deauth` event frame arrives 40–50 ms after the write, before `parse()` runs. Confirmed on a verb that then blocked indefinitely. |
| `[CLI] Result:` withheld while blocked | Absent for the entire block in both runs. |
| BLE cannot rescue a blocking verb | 4 commands (run 1) and 2 (run 2) written during the block: accepted at the GATT layer, never parsed. |
| Event/CLI stream separation | Event JSON never appeared on the CLI characteristic across a 3-command capture. |
| Event IDs monotonic, gap-free | ids 61→66 across three commands. |
| `webui -bg` / `webui -off` | `-bg` returned in 357 ms without holding the screen; `-off` returned `WebUI stopped` and reclaimed memory (DMA largest 6,900 → 19,444). |
| Simultaneous BLE + AP + WebUI | `systeminfo` answered over BLE with the AP up; free heap 14,951, DMA largest 6,900. |
| `RADIO_BLE_MIN_DMA_BLOCK` = 15 KB | `radio_mem.h:32`. |
| Menu exit paths | `loopOptions` breaks on `check(EscPress)` for non-main menus (`display.cpp:647`); `addOptionToMainMenu()` also pushes a `Main Menu` option calling `backToMenu()` (`utils.cpp:27-30`). Either works. |

---

## Resolved

### ISSUE-8 — `blespam fastpair_*` emits malformed adverts and produces no popups

**Status:** RESOLVED in `c9c43c03` · **Severity was:** high (the verb's **default**
type was a no-op) · **Verified** 2026-07-29 · **Fix proven on hardware** 2026-07-29

`blespam fastpair_regular` transmitted nothing a phone would act on. Confirmed
end-to-end against real handsets, and the cause was a container mismatch in the
payload, not a tuning problem.

**Proven over the air.** A BLE sniffer on a laptop captured what the device actually
transmits during `blespam fastpair_regular 200` versus `blespam android 200`. No
handset, notification setting or inference involved:

```
fastpair_regular  ->  mfr_data={771: '2cfe06162cfe000047020ac3'}   x7
                      adverts carrying 0xFE2C service data: 0
android           ->  svc_data={'fe2c': '9adb11'}                  x6   (correct)
                      adverts carrying 0xFE2C service data: 6
```

**Company ID 771 is `0x0303`** — that is `buffer[0]`/`buffer[1]` being consumed as a
little-endian manufacturer identifier. The remaining bytes map onto the intended
payload exactly:

| Buffer bytes | Intended meaning | Actually transmitted as |
|---|---|---|
| `03 03` | AD length 3, type 0x03 | **company ID 0x0303** |
| `2C FE` | UUID 0xFE2C | manufacturer payload |
| `06 16 2C FE` | AD length 6, Service Data, 0xFE2C | manufacturer payload |
| `00 00 47` | 24-bit model ID | manufacturer payload |
| `02 0A C3` | AD length 2, Tx Power | manufacturer payload |

**Handset corroboration.** Two runs of `blespam fastpair_regular 30` with an Android
and an iPhone on their Bluetooth settings screens produced **no popup on either**.
The control, `blespam apple 30`, produced **"Setup New iPhone" on the iPhone**
immediately — so the radio, the transport swap and the spam loop all work; only the
FastPair payload is wrong.

**Console.** Every FastPair run floods `E NimBLEAdvertisementData: Data length
exceeded` at ~1.9 per popup — 17 for `count=10`, 57 for `count=30`, reproducible.
`blespam apple` produces **zero** of these.

**Cause.** `createFastPairAdvertisement()` (`BLE_Suite.cpp:3670-3685`) builds a buffer
that is already a sequence of complete **AD structures**:

```c
buffer[0]=0x03, [1]=0x03, [2]=0x2C, [3]=0xFE   // len 3, type 0x03 (16-bit Service UUID) = 0xFE2C
buffer[4]=0x06, [5]=0x16, [6]=0x2C, [7]=0xFE   // len 6, type 0x16 (Service Data)        = 0xFE2C
buffer[8..10]                                   // 24-bit model ID
buffer[11]=0x02,[12]=0x0A,[13]=0xC3            // len 2, type 0x0A (Tx Power Level)
```

That is raw advertising data. It is then passed to
`pAdvertising->setManufacturerData(fpData, sizeof(fpData))`
(`BLE_Suite.cpp:3473`), which wraps the bytes in **another** AD structure of type
`0xFF` (Manufacturer Specific Data). The result is FastPair AD structures nested
inside a manufacturer-data container — not a FastPair advert, so no Android device
will act on it. The length errors are a second-order symptom of the double wrapping.

The generic engine gets this right: it builds a `NimBLEAdvertisementData` and calls
`pAdvertising->setAdvertisementData(*advertisementData)` (`ble_spam.cpp:1518`).

**Why this matters more than it looks.** `fastpair_regular` is the **default** value
of the verb's `type` argument (`attack_commands.cpp:188`), so a bare `blespam` from
the app is a silent no-op — and per ISSUE-7 it still reports `[CLI] Result: TRUE`.

**Fix:** build the payload with `NimBLEAdvertisementData` (`setServiceData` for
`0xFE2C` plus the model ID) and apply it via `setAdvertisementData()`, matching the
generic engine. Do not hand pre-encoded AD structures to `setManufacturerData`.

**App guidance until fixed:** default to `apple`, and hide or disable the four
`fastpair_*` options.

**The Android caveat, now resolved.** Android only surfaces Fast Pair notifications
with Google Play Services' "Nearby device scanning" enabled, so an absent popup alone
proves nothing. The packet capture settles it from both directions: `android` puts 6
well-formed `0xFE2C` service-data adverts on the air from the same device in the same
session, and that phone showed no popup for those either. **So the test handset's
Fast Pair notifications are simply off** — which is a phone-side condition, not a
firmware fault, and `blespam android` should be considered working at the radio
level. `fastpair_*` transmitted zero valid adverts under identical conditions.

**Fix (`c9c43c03`).** Build the advert with `NimBLEAdvertisementData::addData()` +
`setAdvertisementData()` instead of `setManufacturerData()`, matching the path that
was already working. Root cause confirmed by direct comparison, not inference:
`createFastPairAdvertisement` (`BLE_Suite.cpp:3670`) emits a buffer **byte-for-byte
identical** to `Google_Data[14]` in `GetUniversalAdvertisementData`
(`ble_spam.cpp:311-330`), differing only in the Tx power value. Same bytes, different
container, opposite result on air.

**Proving test** (bare devkit, 2026-07-29, `sniff2.py "blespam fastpair_regular 400" 30`,
run twice on the fixed binary):

```
service-data UUIDs seen:   0xfe2c  x13        (first run)
service-data UUIDs seen:   0xfe2c  x8         (second run, after rebuild)
manufacturer company IDs:  0x0303 absent from both
samples: sd={'fe2c': '000047'} / '00000a' / '0000f0' / '000048' / '000006'
```

Before the fix the same command produced **0** adverts carrying 0xFE2C service data
and 7 carrying company ID 0x0303. The 3-byte service-data payloads are model IDs and
they vary across the model list, as intended.

---

### ISSUE-9 — after some `blespam` types the device advertises without its name or service UUID

**Status:** RESOLVED in `c9c43c03` · **Severity was:** high (the app could not
reconnect) · **Verified** 2026-07-29 · **Fix proven on hardware** 2026-07-29

After `blespam ibeacon`, `samsung` or `windows`, the device stopped advertising as
`Bruc`. It was **not** stranded — the BLE API resumes correctly and the device stays
fully responsive — but its advertisement loses both the local name and the service
UUID, so any client discovering by name cannot find it.

**This entry originally claimed the device was stranded and required a physical
reset. That was wrong**, and the error was in the test, not the firmware: discovery
was done with `find_device_by_name("Bruc")`. Connecting *by address* instead reaches
a healthy device:

```
AA:92:BE:77:04:74  name=None  uuids=[]  mfr=[117]      <- stale Samsung data from the spam
*** HAS THE BRUCE CLI CHARACTERISTIC ***
REPLY: Uptime: 00:03:34
```

**The swap completes normally.** The stage markers show the whole sequence running to
the end, with heap fully restored:

```
swap suspend-pre -> suspend-post -> attack-pre -> attack-post -> swap resume-pre
E NimBLEAdvertisementData: Cannot add UUID, data length exceeded!     <- name/UUID dropped
[BLE_API] setup: adv start=1 isAdvertising=1                          <- advertising IS on
swap resume-post   heap 80,927  DMA 31,732                            <- clean restore
```

**Cause — the spam's advertisement payload is never cleared.**
`bleSpamDeinitAdvertiser()` (`ble_spam.cpp:1427-1443`) calls `BLEDevice::deinit()`,
which defaults to `clearAll = false` and leaves the advertising payload in place.
`bleApiResume()` then calls `bleApi.setup()`, which tries to add the `Bruc` name and
the service UUID **on top of** the leftover spam data. A BLE advertisement has 31
bytes; when the remnant is large the additions do not fit and NimBLE silently drops
them.

That arithmetic predicts exactly which types fail:

| Type | Leftover payload | + flags | + UUID (4 B) + name (6 B) | Fits 31 B? | Observed |
|---|---|---|---|---|---|
| `apple` | 4 B mfr | 9 | 19 | ✅ | recovers |
| `android` | 3 B service data | 8 | 18 | ✅ | recovers |
| `fastpair_*` | 14 B mfr | 19 | 29 | ✅ | recovers |
| `windows` | 21 B mfr | 26 | 36 | ❌ | loses name |
| `samsung` | 24 B mfr | 29 | 39 | ❌ | loses name |
| `ibeacon` | 18-char name | 23 | 33 | ❌ | loses name |

All six data points are consistent. This also explains the
`Cannot add UUID, data length exceeded!` line that appears at resume in the failing
runs and not the passing ones.

**Second defect in the same path: the BT MAC is never restored.**
`bleSpamRestartAdvertiserForMac()` sets the interface address with
`esp_iface_mac_addr_set(mac, ESP_MAC_BT)` (`ble_spam.cpp:1454`) and nothing undoes it,
so after any spam the device advertises under the last random MAC the spam used.
An app that pins a peer address will also lose the device, independently of the name.

**App guidance.** Do not discover by name alone. Match on the **service UUID**
`4371ec0b-3d43-49f9-b731-7c72a4a7bb91`, or cache the peer address and reconnect to it
— but note the address itself changes across a spam, so the service UUID is the only
stable identifier. As a fallback, connect to candidates and probe for the CLI
characteristic, which is how this was diagnosed.

**Fix:** clear the advertising payload in `bleSpamDeinitAdvertiser()` before handing
the radio back (and restore the original BT MAC), so `bleApi.setup()` starts from an
empty advertisement.

**Fix (`c9c43c03`).** Two separate defects on the same path.

*The dropped name and UUID.* Root cause confirmed in the library source, upgrading the
earlier arithmetic to proof: `NimBLEDevice::deinit(bool clearAll = false)` only deletes
`m_bleAdvertising` inside `if (clearAll)`. `bleSpamDeinitAdvertiser` called the
one-argument form, so the advertising singleton and its `m_advData` survived and
`bleApi.setup()` appended the name and service UUID on top of the spam remnant.
`pAdvertising->clearData()` is now called at teardown. `clearData()` was chosen over
switching to `deinit(true)` because it zeroes `m_advData`/`m_scanData` and nothing
else, whereas `deinit(true)` would also delete `m_pServer`.

Corroborating: `BLEStateManager::deinitBLE()` (`BLE_Suite.cpp:327`) already calls
`deinit(true)`, which is why the FastPair engine never showed this symptom.

*The leaked BT MAC.* **Both** engines rotate the address and neither restored it —
`ble_spam.cpp`'s `bleSpamRestartAdvertiserForMac` and `BLE_Suite.cpp`'s
`fastPairRotateAddress`. The first fix attempt covered only `ble_spam.cpp` and the
gap was caught by the test below, which showed FastPair still leaking. Each engine now
snapshots the factory address before its first override and restores it on teardown.

**Proving test** (bare devkit, 2026-07-29). Discovery deliberately by **service UUID**,
never by name, so a missing name is reported as a finding rather than as a missing
device — the harness failure that caused this entry's original wrong diagnosis:

```
type               name     service UUID   BT MAC
apple              Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
android            Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
samsung            Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
windows            Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
ibeacon            Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
fastpair_regular   Bruc     present        1C:DB:D4:5E:D7:39 -> unchanged
```

All six pass, where `samsung`, `windows` and `ibeacon` previously lost the name and
every type previously leaked the MAC. Two full back-to-back sweeps were run with
`/dev/ttyACM0` captured throughout: no panic, no reset, `millis()` monotonic across
every sample in both captures (150s->592s over 46 samples, and 431s->592s over 19),
and heap returning to ~81,000 with the DMA block at 31,732 after every single run.

---
