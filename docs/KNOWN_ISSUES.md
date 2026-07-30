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
· **Mitigated but deliberately not closed** — see §Fix applied below

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

**CONFIRMED 2026-07-29: `evilportal` crashes too, under load.** The hypothesis below
was tested and held. With a client associated and portal pages being requested, the
device asserted after ~11 minutes with the **identical** assertion, and an ELF-matched
backtrace (`b02178b48`) whose shape is the same cross-task SPI release:

```
assert failed: xTaskPriorityDisinherit tasks.c:5156 (pxTCB == pxCurrentTCBs[ xPortGetCoreID() ])

_serialCmdsTaskLoop          serialcmds.cpp:84        <- the serial task
handleSerialCommands         serialcmds.cpp:66
SimpleCLI::parse             SimpleCLI.cpp:55
evilportalCmdCallback        attack_commands.cpp:52   <- fork code
EvilPortal::loop             evil_portal.cpp:305
EvilPortal::drawScreen       evil_portal.cpp:429
drawMainBorderWithTitle      display.cpp:1049
drawMainBorder               display.cpp:1041
drawStatusBar                display.cpp:948
drawBatteryStatus            display.cpp:1108
tft_logger::drawRoundRect    tftLogger.cpp:349
TFT_eSPI::drawRoundRect      TFT_eSPI.cpp:2650
TFT_eSPI::drawCircleHelper   TFT_eSPI.cpp:2429
TFT_eSPI::end_tft_write      TFT_eSPI.cpp:114
SPIClass::endTransaction     SPI.cpp:211
spiEndTransaction            esp32-hal-spi.c:1336     -> assert
```

Note `drawStatusBar` → `drawBatteryStatus` in the serial task's stack: this is the
**same status bar the main loop redraws on its 30 s timer**, which is precisely the
collision the entry predicted. So the trigger is not `drawArc` specifically — any
sustained drawing from the serial task will do, and `deauth` was simply the fastest
way to get there.

**Revised scope: `deauth` and `evilportal`-under-load both crash, with the same
assertion.** The idle result below stands — `evilportal` survived 100 s with nothing
associating — which means **an idle test of any of these verbs proves nothing**. The
survivors in the table were all tested idle for 90 s.

**Only `deauth` crashes when idle.** That is a narrower blast radius than feared, and
the shape of the exception is informative. `wifi_atk_menu`'s `loopOptions` path drives `drawArc`
**continuously**. Every verb that survived redraws only on an event:
`ScrollableTextArea` redraws on input (`ap_info`), and `EvilPortal::loop()` calls
`drawScreen()` only when `shouldRedraw` is set — on a credential capture, a WiFi
restart, or a button press (`evil_portal.cpp:290-320`).

So the working hypothesis is that the collision needs *sustained* drawing from the
serial task, not merely *any* drawing. Two consequences, both unproven:

- A quiet UI is not a safe UI, it is an unlikely-to-collide one. A clean 90 s window
  is **not** proof — `deauth` itself survived 70+ s on its first run before dying.
- ~~**`evilportal` was tested idle**… That case has **not** been tested and is the
  obvious next experiment.~~ **Done 2026-07-29 — the prediction was correct.** See the
  confirmation above. This is recorded rather than deleted because it is the one case
  where a stated hypothesis was tested and held, which is worth as much as the
  rejections in ISSUE-11 and ISSUE-12.

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

**Fix applied in `2d9422ea`; two independent confirmations 2026-07-29.** The mutex was
scoped to the *call* when the thing needing protection is the *transaction* — see that
commit for the full mechanism. On ELF `5186685c0fdf19c2`:

| Verb | Before | After |
|---|---|---|
| `deauth` | crashed 2/2 in 20–70 s | **21 min clean** |
| `blesniffer` | only ever idle-tested 90 s | **13 min clean** |

`blesniffer` matters as a second case because `BleSuiteMenu()` is also a
`loopOptions`, so it reproduces the same continuous-redraw-from-the-serial-task
condition through a different verb. Neither run produced an `assert failed`,
`Backtrace:`, `rst:0x` or `ESP-ROM` marker.

**Kept OPEN deliberately.** This is a probabilistic result against a race, not a proof.
The `evilportal`-under-load case remains unconfirmed for an unrelated reason: that run
starved itself to 163 bytes and exited before the drawing could collide (ISSUE-21).

**The symbol is `tftMutex`** (`lib/TFT_eSPI/TFT_eSPI.cpp:72`, created at `:646`), named
here because the paragraphs above describe the mechanism without ever writing it down,
so `grep tftMutex docs/` found nothing and a 2026-07-30 audit wrongly concluded no fix
existed. It guards six entry points: `begin/end_tft_write` (`:95`/`:126`),
`begin/end_nin_write` (`:109`/`:143`), `begin/end_tft_read` (`:164`/`:187`) and
`begin/end_touch_read_write` (`Extensions/Touch.cpp:30`/`:51` — compiled here, because
`pins_arduino.h:77` defines `TOUCH_CS` as `-1` and the guard is `#ifdef`).

**The mutex itself is upstream, not fork work.** It arrives in `517cec01` (the mQuickJS
runtime change, #1989) as a *call*-scoped mutex. `2d9422ea` re-scoped it to the SPI
transaction and extended it to the `nin_write` and `tft_read` pairs, which upstream left
with no protection at all.

**A latent trap worth knowing about — audited 2026-07-30, inert today.** The library has
**32** live `begin_tft_write()` calls against **57** live `end_tft_write()` calls,
because 20 sites comment the begin out so the Sprite class can reuse the function
(`:2330, 2404, 2472, 2548, 2600, 2647, 2668, 2694, 2716, 2758, 2815, 2839, 2862, 2886,
3280, 3318, 3411, 3551, 3938, 5355`) and `drawArc:4133` sets `inTransaction` with no
begin. So the trailing per-call `xSemaphoreGiveRecursive` runs ~25 times more often than
its take.

That is **harmless as the code stands**: every unmatched give lands on recursion count 0,
where FreeRTOS `xQueueGiveMutexRecursive` returns `pdFAIL` and does nothing. Turning it
harmful needs an unmatched give while an *outer* transaction holds count 1, and no such
nesting exists here — verified: **zero** `startWrite()`/`endWrite()` call sites in `src/`
and `boards/`; `TFT_eSprite` unused in `src/`, so `lockTransaction` (its only true-setter
is `Extensions/Sprite.cpp:42`) is always false and `inTransaction = lockTransaction`
always restores to false; and the only internal caller of a comment-out composite is
`drawString` (5 × `drawRect` at `:5781-5793`, in its padding-debug block), which holds no
transaction anywhere in `:5584-5810`.

**It goes live the moment anyone adds `startWrite()`/`endWrite()` to `src/`.** Don't,
without re-reading this.

**Retired for the HEADLESS portal path only — 2026-07-30, ELF `76d42c72f2b4a8a4`.**
`evilportal -bg` was run under load with a client associated and pages being fetched,
and the assertion **did not reproduce**: no `xTaskPriorityDisinherit`, no SPI mutex
failure, no `Backtrace:`. That is the expected result rather than a lucky one — the
background path deliberately draws nothing from the serial task, which removes the
precondition entirely. It died of memory instead (ISSUE-25).

**The blocking `evilportal` verb is unchanged and still carries this defect.** It still
runs `EvilPortal::loop()` → `drawScreen()` on the serial task, which is the exact stack
in the backtrace above. Nothing in the headless work touched it. The same holds for
`deauth`, `karma`, `blesniffer`, `ap_info` and `pwngrid`, none of which gained a
headless entry point.

---

### ISSUE-2 — `settings` with no arguments returns nothing over BLE

**Status:** RESOLVED in `b1c825c8` · **Severity:** low · **Verified** 2026-07-29

`settings` with no arguments is documented as "View all the current settings", but
over BLE it returns 5 bytes — `\r\n` plus the `# ` prompt plus EOT — and no JSON.

Cause: `settingsCallback` calls `serializeJsonPretty(jsonDoc, Serial)`
(`src/core/serial_commands/settings_commands.cpp:19`) — the `Serial` object, not
`*serialDevice`. When the BLE API is armed, `serialDevice` points at the GATT service
(`ble_api.cpp:63`), so the config never reaches the client. A correlated 22 s USB CDC
capture during the command did not show the JSON on that port either.

Single-field reads are unaffected: `settings bright` → `bright = 100`.

**Fix:** `Serial` → `*serialDevice` in that one call.

**Fix (`b1c825c8`).** Serialise to a `String` and print it to `*serialDevice`.
Verified 2026-07-29 on ELF `4bdcd1dc364fd2cf`: `settings` over BLE returns **1,917
bytes** of config JSON with `eot=True`, where it previously returned 5.

---

### ISSUE-3 — `battery_pct` and `charging` are fabricated with no PMU fitted

**Status:** PARTIALLY FIXED in `b1c825c8` (I²C storm gone; reporting still wrong) · **Severity:** medium (blocks any battery UI) · **Verified** 2026-07-29

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

**Partial fix (`b1c825c8`).** `PPM.init()`'s result is now stored and both
`getBattery()` and `isCharging()` return early when no PMU is fitted.

*What this fixed:* the console storm. Roughly ten
`i2c_master_transmit failed: ESP_ERR_INVALID_STATE` lines every 22 s, forever, on the
same port a panic backtrace has to appear on. Verified 2026-07-29: **0 bytes** of
console output across a 55 s idle capture, against ~25 expected before.
`isCharging()` also now correctly reports `false` instead of `true`.

*What is still wrong:* `battery_pct` still reports `1`. A sentinel was rejected
deliberately — `getBattery()` is assigned to a `uint8_t` at `display.cpp:947` and
`BatteryService.cpp:16`, so `-1` would render as **255%**. Reporting the absence
honestly needs a `batteryPresent()` (or equivalent) in `include/interface.h`, which
all **24** board implementations would then have to provide. That is a cross-board
change, not a quick win, and it is not done. **The app must still not show a battery
UI.**

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

**Status:** RESOLVED 2026-07-30 · **Severity:** low · **Verified** by code

`deauthCmdCallback` ignores its `target` argument entirely and calls `wifi_atk_menu()`
(`attack_commands.cpp:152-155`). The CLI advertises a parameter that has no effect.

Superseded in practice by ISSUE-1 — the verb cannot be used at all right now.

**Fix applied 2026-07-30, ELF `411d7e151dbc2356`.** A non-empty `target` is now
rejected with an explicit error instead of being silently dropped. Honouring it was
not an option — `wifi_atk_menu()` takes no target and picks one on the device — so
the honest choice was to stop advertising a capability that does not exist.

Verified on hardware over BLE:

```
$ deauth AA:BB:CC:DD:EE:FF
ERROR: 'deauth <target>' is not supported — wifi_atk_menu() selects the target
on the device. Run 'deauth' with no argument.
```

**Unexpected side benefit:** the rejection returns in **63 ms** and never opens the
menu, so this form of the verb no longer blocks the serial task and no longer carries
ISSUE-1's crash risk. `deauth` with no argument is unchanged and still does both.

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

**Status:** RESOLVED 2026-07-30 · **Severity:** high (app cannot detect failure) · **Verified** 2026-07-29

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

**Fix applied 2026-07-30, ELF `411d7e151dbc2356`.** Both halves of that direction,
plus a real defect found while doing it.

*A new `attack_result` event frame.* All six callbacks now route through
`runInteractiveAttack()`, which times the call, restores the device state and emits:

```json
{"id":10,"type":"attack_result","verb":"deauth",
 "outcome":"rejected_unsupported_target","elapsed_ms":0,
 "wifi_mode":0,"free_heap":80547}
```

**`outcome` is deliberately `completed`, never `success`.** These verbs open an
interactive menu; finishing one means the operator left it, not that an attack
worked. Anything stronger would just repeat the old lie in a new field. What the app
gains is `elapsed_ms`, `wifi_mode` and `free_heap` — enough to recognise the failure
shape that started this entry, since an attack that "completes" in 30 ms did not run.

*A genuinely knowable outcome, for the one verb that has one.* `ReverseShell()` was
`void`; it now returns `bool` and `reverseshellCmdCallback` propagates it, printing
`ERROR: reverseshell could not start its AP` and returning **false**.

**Root cause of the original observation, found and fixed.** The
`create(): passphrase too short!` in this entry was not bad luck or a missing config
— `reverseShell.cpp` called `WiFi.softAP("BruceShell", "bruce")`, and **WPA2 rejects
any passphrase under 8 characters**. The AP could therefore *never* start, on any
build, for anyone. The passphrase is now `REVERSE_SHELL_AP_PASSWORD`
(`"bruceshell"`, 10 chars), defined once in the header so the on-screen text cannot
drift from what is handed to `softAP()`. **The AP-up path is still UNTESTED** — the
verb blocks until an Esc chord once its AP succeeds, so it needs an attended run.

**Verified on hardware** 2026-07-30 — `[CLI] Result: FALSE` from an attack verb, which
had never happened before:

```
$ deauth AA:BB:CC:DD:EE:FF
  -> ERROR: 'deauth <target>' is not supported ...
  +1.44s {"id":10,"type":"attack_result","verb":"deauth","outcome":"rejected_unsupported_target",...}
  +1.45s {"id":11,"type":"log","line":"[CLI] Result: FALSE","level":"info"}
```

⚠️ **`attack_result` is a new frame type.** `BRUCELINK.md` and the API contract both
state that only `state`, `log`, `ble_progress` and `ble_result` are emitted. That is
now out of date, and the maritest `vendor/` copy of the contract is stale.

---

### ISSUE-10 — `blespam samsung` transmits Galaxy Buds packets with no Flags AD structure

**Status:** RESOLVED in `b1c825c8` · **Severity:** low · **Verified** 2026-07-29 · **Pre-existing,
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

**Fix (`b1c825c8`).** `setFlags(0x06)` now runs only on the Galaxy Watch branch,
whose 15-byte payload leaves room for it. The Buds branch omits it — real Galaxy Buds
advertise no Flags structure either, so this is also the more faithful packet.
Verified 2026-07-29: `blespam samsung 30` produced **0** `Data length exceeded`
(was 30/30), and a capture during `blespam samsung 60` still shows the payload on air
under company ID 117 (`42098102141503210109d3070104063c948e00000000c700`) — removing
the flags did not stop transmission.

---

### ISSUE-11 — one unexplained reboot during a back-to-back spam sweep, not reproduced

**Status:** OPEN · **Severity:** unknown · **Observed once** 2026-07-29 · **No console
capture of the event** · **Not reproduced in 2 subsequent attempts**

> **"No console capture" is no longer a dead end — 2026-07-30.** `crashlog` reads the ELF
> core dump a panic stores in flash, so a future occurrence leaves evidence whether or not
> anyone was watching (see §Shippable in `TEST_STATUS.md`). **A dump survives a firmware
> flash** — `upload` writes 0x0/0x8000/0x10000 and the coredump partition is at 0xFF0000 —
> but the next crash overwrites the last, so read it before provoking another.
>
> **And a real one was recovered on the very first read**, from a crash nobody had seen:
>
> ```
> crash: task=loopTask pc=0x400556d2 elf=a4bc5d735 depth=10 corrupted=no
> crash: bt=0x400556d2 0x422992e2 0x4228e855 0x4228e895 0x4214b1ad 0x421f113b
>            0x4207adc5 0x420aa58e 0x4214ec6c 0x40385bbd
> ```
>
> **It is not an ISSUE-1 crash**: both of those are in `_serialCmdsTaskLoop`, the serial
> task, while this is **`loopTask` — the main loop**. Whether it is *this* entry's reboot is
> **unknown**; it is simply the most recent panic the partition held.
>
> ⚠️ **Undecoded, and deliberately not guessed at.** `app_elf_sha256` is `a4bc5d735` and the
> build that produced it is gone from `.pio`, so decoding against any current ELF would be
> fiction by this repo's own rule. Recoverable only by rebuilding the commit whose app sha
> is `a4bc5d735` and confirming the match first. The raw record is kept above so that
> remains possible.

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

**Second occurrence, 2026-07-29 (later session).** The device rebooted unprompted again
while blocked in `blesniffer` with the WiFi stack torn down (see ISSUE-19). Reported by
the user; **no console was being captured at that moment**, so again there is no
backtrace and no reset reason. Uptime afterwards confirmed a fresh boot (free 81,187,
largest 31,732). Context is sharper this time — `blesniffer` reaches `loopOptions`,
which draws from the serial task, i.e. the ISSUE-1 mechanism, and the earlier sweep had
only ever left it blocked for 90 s whereas this run was several minutes. **That is a
hypothesis, not a finding**: the capture that would prove or refute it does not exist.
The obvious experiment is to hold `blesniffer` blocked with `usbwatch2.py` running for
the whole window.

**Do not read this as fixed or as caused by `c9c43c03`; neither is established.** It
is recorded because a one-off reset that is not understood is worth recognising if it
recurs. Next step if it does: keep `usbwatch2.py` running for the whole session so the
event is captured rather than inferred.

---

### ISSUE-12 — `webui` starts with almost no margin, and fails silently if anything consumed heap first

**Status:** OPEN · **Severity:** high · **Verified** 2026-07-29 · **Both outcomes
reproduced, with console captures**

From a **fresh boot** the WebUI works: the AP accepts a station, DHCP hands out a
lease, and HTTP serves. From a boot where something has already taken ~18 KB, the same
command fails completely — and reports success either way. The margin is the finding.

**Working case** — fresh boot, free heap 81,339 / largest 31,732 before `webui -bg`:

```
[RAMLOG] stage=webui pre-alloc      heap free= 28988 largest= 20468 dma= 20468
[RAMLOG] stage=before MDNS          heap free= 28728 largest= 20468 dma= 20468
[RAMLOG] stage=after MDNS           heap free= 23068 largest= 14836 dma= 14836
[RAMLOG] stage=webui pre-ws         heap free= 20608 largest= 12788 dma= 12788
[RAMLOG] stage=webui post-begin     heap free= 15392 largest=  7156 dma=  7156
```

No AsyncTCP error. `systeminfo` answered over BLE **with the AP up** — a full 472-byte
reply, `wifi_mode:2`, `free_heap:14140`. The laptop associated and got `172.0.0.3/24`.

**Failing case** — same command, same firmware, after a JS interpreter run had left
free heap at 62,763 instead of 81,391:

```
[RAMLOG] stage=webui pre-alloc      heap free=  9903 minEver= 119 largest= 6132 dma= 1844
[RAMLOG] stage=after MDNS           heap free=  3703             largest= 1652 dma=  628
[RAMLOG] stage=webui pre-ws         heap free=  1235 minEver= 103 largest= 1012 dma=   28
[E][AsyncTCP.cpp:1521] begin(): failed to start task
[RAMLOG] stage=webui post-configure heap free=   943             largest=  756 dma=   28
```

The HTTP server never listens. The AP still beacons — visible in every scan at signal
90-100 — but cannot complete an association, which NetworkManager reports misleadingly:

```
supplicant interface state: inactive -> authenticating
supplicant interface state: authenticating -> disconnected
Error: Connection activation failed: The Wi-Fi network could not be found
```

It does find the AP; it gets to authentication and the device cannot allocate for the
station. Tested with MAC randomisation on and off and with the profile pinned to BSSID
`1E:DB:D4:5E:D7:38` — no effect, consistent with a device-side cause.

**`webui -bg` returns success in both cases**, printing `AP` / `Press ESC to quit`
whether or not a server started. Same family as ISSUE-7. The only in-band signal that
it failed is that nothing subsequently answers on port 80.

**In the failing state the BLE control link dies too**: CLI replies truncate to 1 byte
— command accepted, EOT arrives, payload gone. `webui -off` restores full replies
immediately, which is what establishes causation. Observed `minEver=119`.

**~18 KB is the whole margin.** mDNS alone costs 5.6-5.9 KB and roughly halves the
largest block in both runs. Anything that permanently retains heap before `webui`
starts — see ISSUE-17 — can push it over.

**Correction to this entry's own history:** it originally claimed `webui` cannot start
at all while the BLE API is armed, generalising from the failing run. That was wrong,
and it was caught by a conflicting prior measurement already recorded in §Verified
working ("Simultaneous BLE + AP + WebUI … free heap 14,951"), which the fresh-boot
retest then confirmed at 14,140. Recorded rather than deleted because the failing case
is real and the trap — testing from a dirty baseline — is easy to repeat.

**The margin is far tighter than "~18 KB" — it is decided by under 1 KB.** Measured
2026-07-30 on ELF `76d42c72f2b4a8a4`: three *fresh boots*, the same `webui -bg`, and
the outcome flipped twice. **Neither of the first two logged an AsyncTCP error**, so
both looked like the "working" profile above at the RAMLOG level:

| stage | run 1 — **worked** | run 2 — **failed** | run 3 — **worked** |
|---|---|---|---|
| `first-mainMenu` | 81,103 / dma 31,732 | 81,103 / dma 31,732 | 81,243 / dma 31,732 |
| `webui pre-alloc` | 28,467 / 19,444 | 27,639 / 18,420 | — |
| `after MDNS` | 22,567 / 14,324 | 21,988 / 13,812 | — |
| `webui pre-ws` | 20,103 / 12,276 | 19,523 / 11,252 | 20,231 / 12,276 |
| `webui post-begin` | 14,887 / **dma 6,644** | 14,307 / **dma 6,132** | 15,015 / **dma 6,900** |
| `POST /login` | 302 + cookie, sub-second | **http=000 ×4** (14.3 s, 3.1 s, 2.0 s, 35 s) | 302 + cookie, 1.33 s |

The deciding margin between run 1 and run 2 was **~580 bytes**. The one behavioural
difference: run 1 started the WebUI at `t=38,550 ms` after little navigation, run 2 at
`t=191,370 ms` after the operator had moved around the UI, run 3 at `t=20,327 ms` with
none at all — and run 3 had the largest DMA block of the three.

**Operational rule that follows: issue `webui -bg` as soon after boot as possible, and
do not navigate menus first.** This is ISSUE-17's principle at a much finer grain than
that entry's ~18 KB.

**`dma largest` at `webui post-begin` predicts the outcome better than free heap does**
— 6,900 and 6,644 served, 6,132 did not, while free heap differed by well under 5%
across all three. **SUSPECTED, not verified**: this rests on one set of three runs and
no threshold has been bisected.

**In the failing state the device is one allocation from the BT-controller abort.**
Run 2 logged `E (309086) BLE_INIT: Malloc failed` twice on the console, BLE replies
collapsed to 3 and 1 bytes (ISSUE-16), and `free` after recovery reported
**`minEver=776`**. See ISSUE-25 — that is the same condition which aborts the device
when the log call itself cannot allocate.

**Recovery that works, in this order:** drop the station first (`nmcli con down`), then
`webui -off` over BLE. Heap went 14,307 → 57,435 / dma 19,444. The `webui -off`
executes even while replies are coming back empty.

**Association vs. DHCP — the failures are at different layers, and this entry
previously conflated them.** On 2026-07-29 NetworkManager reported *"The Wi-Fi network
could not be found"*, which reads as an association failure. On 2026-07-30, with
`ipv4.method auto`, it reported instead **"IP configuration could not be reserved"** —
association had *succeeded* and only DHCP failed. Setting a **static address**
(`ipv4.method manual`, `172.0.0.5/24`, no gateway) associated immediately and reached
the device: ICMP 3/3, TCP 80 accepted, HTTP served. Heap after the failed DHCP attempt:
free 14,771, `minEver` 8,415, dma largest 6,644 → 4,596.

⚠️ **The documented remedy was not tried.** §Test harness in `TEST_STATUS.md` already
records `nmcli connection modify <profile> ipv4.dad-timeout 0` for exactly this
symptom, attributing it to ARP duplicate-address detection hanging because the ESP32
does not answer ARP probes. That is a better fix than a static address and it was
missed on the day. **Try `ipv4.dad-timeout 0` before reaching for a static IP.**

**Tried 2026-07-30. It does not work, and the ARP/DAD attribution is wrong.** A fresh
`nmcli` profile created with `ipv4.dad-timeout 0` still failed with *"IP configuration
could not be reserved"*. The NetworkManager journal shows association succeeding
outright and only DHCP failing:

```
device (wlp39s0): supplicant interface state: 4way_handshake -> completed
device (wlp39s0): Activation: (wifi) Stage 2 of 5 (Device Configure) successful.
                  Connected to wireless network "BruceNet"
dhcp4 (wlp39s0): activation: beginning transaction (timeout in 45 seconds)
   ... no lease
```

Association completes; **the device's DHCP server simply does not answer**. Duplicate-
address detection is not involved, so disabling it changes nothing.

**A static address is therefore the actual remedy, not a workaround hiding a DHCP
problem** — the DHCP problem is device-side and unfixed:

```sh
nmcli con mod <profile> ipv4.method manual ipv4.addresses 172.0.0.5/24 \
      ipv4.gateway "" ipv4.never-default yes
```

Verified 2026-07-30: associates immediately, ICMP 3/3 to `172.0.0.1`, `POST /login`
302 + cookie in 0.35 s. **`TEST_STATUS.md` §Test harness has been corrected.**

**One more correction: elapsed uptime is not the variable — navigation is.** This
entry's operational rule says to issue `webui -bg` "as soon after boot as possible".
Counter-example measured 2026-07-30: `webui -bg` fired at **t=717,477 ms (~12 minutes
uptime)** with no operator navigation still reached `webui post-begin` at **dma
largest 6,900** — equal to the best of the three runs tabled above and better than
run 1 (6,644) and the failing run 2 (6,132). A second run at t=36,467 ms also gave
6,900. **Restate the rule as "do not navigate menus before starting the WebUI";
elapsed time is not implicated.**

---

### ISSUE-13 — `encrypt` then `decrypt` fails ~62% of the time, silently

**Status:** RESOLVED in `b1c825c8` · **Severity:** high (silent data loss to the user's eye) ·
**Verified** 2026-07-29 · **Root cause proven, falsifiable test 8/8**

A file written by `encrypt` often cannot be read back by `decrypt` on the same device
with the same password. The failure is silent: an empty reply, identical to the reply
for a wrong password.

**Root cause — a writer/reader format mismatch.** The writer does not zero-pad hex:

```cpp
// src/core/passwords.cpp:162
for (size_t i = 0; i < dataStr.length(); i++) dataStrHex += String(dataStr[i], HEX) + " ";
```

`String(v, HEX)` emits one character for values below 0x10, so byte `0x08` is written
`8`. The reader assumes a fixed 3-character stride:

```cpp
// src/core/passwords.cpp:113-116
for (int i = 0; i < cypertextData.length(); i += 3) {
    uint8_t highNibble = hexCharToDecimal(cypertextData[i]);
    uint8_t lowNibble  = hexCharToDecimal(cypertextData[i + 1]);
```

One short token desynchronises every byte after it. Visible in a real file — note
`7B 8 4B`:

```
Data: 5E D6 13 B0 B4 BE 80 31 F8 93 6D 7C FA 72 7B 8 4B DD 69
```

**Proving test.** Prediction: decrypt succeeds iff every token on the `Data:` line is
2 characters. Eight payloads encrypted and read back, 2026-07-29:

| payload | short token | predicted | actual |
|---|---|---|---|
| alpha | — | ok | **ok** |
| bravo | `2` | fail | **fail** |
| charlie | `2` | fail | **fail** |
| delta | `F` | fail | **fail** |
| echo | `B` | fail | **fail** |
| foxtrot | — | ok | **ok** |
| golf | `F` | fail | **fail** |
| hotel | — | ok | **ok** |

**8/8 agreement**, 5 of 8 failing. Expected failure rate for an N-byte ciphertext is
`1 - (240/256)^N` — about 70% at N=19.

**No data is lost on disk**; the bytes are all present and a tokenising reader would
recover existing files. Fix either end: zero-pad in `encryptString`, or split on
whitespace in `readDecryptedFile`. Padding the writer alone would leave existing
files unreadable, so the reader should be fixed regardless.

**Worth stating plainly to users:** the algorithm is XOR with an MD5-derived key, 10
passes (`Algo: XOR`, `KeyDerivationAlgo: MD5` in the file header). It is obfuscation,
not encryption, and the app should not present it as the latter.

**Fix (`b1c825c8`).** Both ends. `encryptString` zero-pads to two characters and
casts through `uint8_t` (which also stops a byte over 0x7F sign-extending into a
multi-character token); `readDecryptedFile` tokenises on whitespace instead of
stepping a fixed three. **The reader fix is the important one** — it recovers every
unpadded file already on disk, which padding the writer alone would have left
unreadable.
Verified 2026-07-29 with `tools/ble_spike/cryptotest.py`: **8/8 payloads round-trip**,
`short tokens []` empty for every one. Before: 5 of 8 failed.
Still XOR with an MD5-derived key — obfuscation, not encryption. The app should not
present it as the latter.

---

### ISSUE-14 — `settings <field> <value>` silently does nothing for most fields

**Status:** RESOLVED in `b1c825c8` · **Severity:** high (writes report success and change nothing) ·
**Verified** 2026-07-29 · **Tested against a control**

`settingsCallback` validates the field name against `bruceConfig.toJson()`, but only
**14** fields are actually wired to a setter: `priColor`, `rot`, `dimmerSet`,
`bright`, `tmz`, `soundEnabled`, `wifiAtStartup`, `webUI`, `wifiAp`, `wifi`,
`wigleBasicToken`, `wdgwarsApiKey`, `devMode`, `disabledMenus`
(`settings_commands.cpp`). Every other serialised field passes validation, matches no
branch, and reaches the closing `return true`.

**Tested on device**, with a whitelisted field as the control:

```
$ settings bleApiAutoStart        ->  bleApiAutoStart = 1
$ settings bleApiAutoStart 0      ->  (empty reply)
$ settings bleApiAutoStart        ->  bleApiAutoStart = 1     <- unchanged
$ settings bright                 ->  bright = 100
$ settings bright 60              ->  (empty reply)
$ settings bright                 ->  bright = 60             <- changed
$ settings nosuchfield 1          ->  Invalid field name: nosuchfield
```

A successful write and a silently discarded one return the **same empty reply**. Only
a name that is absent from the JSON produces an error. The app cannot tell a write
that took effect from one that did not.

`bleApiAutoStart` being unwritable is what makes ISSUE-12 untestable from the CLI.

**Fix (`b1c825c8`).** The branches are now an `else if` chain with a `written`
flag. An unmatched field reports `Read-only setting, not writable from the CLI:
<name>` and returns false; a successful write echoes `<name> = <value>` instead of
staying silent.
Verified 2026-07-29:

```
settings bleApiAutoStart 0  ->  Read-only setting, not writable from the CLI: bleApiAutoStart
settings bleApiAutoStart    ->  bleApiAutoStart = 1      <- provably unchanged
settings bright 60          ->  bright = 60
settings bright             ->  bright = 60              <- provably changed
settings nosuchfield 1      ->  Invalid field name: nosuchfield
```

---

### ISSUE-15 — the JS interpreter runs scripts but has no return channel

**Status:** RESOLVED 2026-07-30 (output); **errors still invisible** · **Severity:** high (no output, no errors, no result) ·
**Verified** 2026-07-29

`js run_from_buffer` executes correctly — proven by side effect, not by output:

```
$ js run_from_buffer 200
  storage.write("/js_ran.txt", "JS_EXECUTED");
$ cat /js_ran.txt   ->  JS_EXECUTED
```

But nothing a script prints, and no error it raises, ever reaches the caller. The two
print bindings both bypass `serialDevice`:

- `log()` → `js_print` (`globals_js.cpp:266-285`) writes to C `stdout` via
  `fwrite`/`putchar`. Not `Serial`, not `serialDevice`; observed on neither channel.
- `serial.print()` / `serial.println()` → `internal_print_mq`
  (`serial_js.cpp:8-40`) writes to `Serial` and optionally `tft` — the USB console at
  best, never the BLE client.

**Errors are equally invisible, and indistinguishable from success.** Three runs, all
returning a byte-identical 52-byte reply (`Reading input data from serial buffer
until EOF` + prompt):

| script | outcome | BLE reply |
|---|---|---|
| `storage.write(...)` | ran, file created | 52 bytes |
| `this is not valid javascript @@@` | syntax error | 52 bytes |
| `nosuchfunction(1);` | reference error | 52 bytes |

A `ReferenceError` for bare `print` was seen once on the USB console, so errors do
reach `Serial` in some paths — but never the app.

**Consequence:** the app can ship payloads but cannot read a result or detect a
failure. The only working return path is a side effect the app then polls for, e.g.
write a file and `cat` it.

**Naming trap:** bare `print`/`println` are **badusb HID** natives
(`native_badusbPrint`, `mqjs_stdlib.h`), not console output. A script calling
`print("x")` types keystrokes into whatever host the device is plugged into.

**Fix applied 2026-07-30, ELF `411d7e151dbc2356` — script output now reaches the app.**
Both bindings were redirected: `js_print` (`globals_js.cpp`) no longer writes to C
`stdout` via `fwrite`/`putchar`, and `internal_print_mq` (`serial_js.cpp`) no longer
writes to `Serial`. Both now emit an event frame prefixed `[js] `.

**They go to the event stream, not to `serialDevice`, and that is the load-bearing
design decision here.** Routing them to the CLI characteristic was tried first and is
wrong: scripts run on the **interpreter task**, so their output appears *after* the
`js` verb has already written its reply and its `0x04` EOT. Measured on hardware:

```
CLI characteristic:  52 bytes, "Reading input data from serial buffer until EOF" + EOT
  +3.00s  {"id":2,"type":"log","line":"[CLI] Result: TRUE","level":"info"}
  +3.54s  {"id":3,"type":"log","line":"[js] HELLO_FROM_JS","level":"info"}
  +3.54s  {"id":4,"type":"log","line":"[js] 42","level":"info"}
```

The output lands **540 ms after the response boundary**. On the CLI characteristic
those bytes would have been injected into whatever command came next and
desynchronised its framing — the same hazard that makes `display start` unusable. The
`6*7` → `42` also confirms non-string values stringify correctly, which the old
`JS_PrintValueF` stdout-only path did separately.

*What is still wrong:* **errors remain invisible.** A syntax error or a
`ReferenceError` still produces no frame and is still indistinguishable from success
at the client — only `print` output was fixed. The three-run table above stands for
the error cases.

*Consequence for the app:* it must subscribe to the **event** characteristic to read
script output; the `js` verb's own reply will never contain it. Output is
asynchronous, so correlate by `id` ordering rather than expecting it before
`[CLI] Result:`.

---

### ISSUE-16 — BLE replies truncate silently under heap fragmentation, with no marker

**Status:** OPEN · **Severity:** medium · **Verified** 2026-07-29 · **Non-deterministic**

After a `webui` start/stop cycle the largest free block does not return to baseline —
31,732 bytes at boot versus 11,252 after — and BLE replies begin to lose data with no
indication. The documented `[TRUNCATED: device low on memory]` marker never appeared.

Three identical `ls /` calls in succession returned **297, 257 and 297 bytes**; the
257-byte reply silently dropped a filename and left a bare size on its own line:

```
PortalTemplates	<DIR>
1456                       <- name gone, entry mangled
bruce.conf	1912
```

`storage stat /bak.bruce.conf` returned a partial reply and **no EOT at all**, timing
out after 25 s with the `regular file` and `Modify:` lines missing.

**No data was lost on the device** — `bak.bruce.conf` was confirmed present at its
correct size of 1932 bytes by a direct `storage stat`, and a full `ls /` after reboot
matched the session-start listing exactly. This is a transport defect, not a
filesystem one. Worth stating because a truncated `ls` reads exactly like file loss.

**A reboot fully restores it**: free 81,391, largest block 31,732, `ls /` back to a
complete 297 bytes.

**Consequence for the app:** a short reply is not proof of a short answer, and a
missing EOT is not proof the device is wedged. The app should treat a missing EOT as
"retry", and should not render a file listing as authoritative after heavy WiFi use.

**Reproduced deliberately 2026-07-29, with the cause identified.** With the BLE API
armed, the AP up and the WebUI running (15,167 free / DMA 6,900), a single HTTP
request for real content drove free heap to **812 bytes**. At that point BLE replies
came back **empty** — `free` returned 3 bytes, `systeminfo` returned 1 — with the EOT
terminator present and **no `[TRUNCATED: device low on memory]` marker**.

So the marker does not fire in the exact condition it exists for. The reply is not
being truncated mid-chunk by a failed notify; the device cannot allocate the reply
**string** in the first place, so there is nothing to truncate and nothing to flag.

The commands still executed: `webui -off` issued in that state returned
`WebUI stopped` and recovered the heap to 57,295. **An empty reply means "the device
could not build an answer", not "the device did not act".** An app must not retry on
an empty reply — the side effect has already happened.

**Reproduced again 2026-07-30** on ELF `76d42c72f2b4a8a4`, and the "still executed"
property held a second time. In the ISSUE-12 run-2 failing state, `free` returned
**3 bytes** and `uptime` returned **1 byte**, both with `eot=True` and **no**
`[TRUNCATED: device low on memory]` marker. `webui -off` issued in that same state
returned a full 18-byte `WebUI stopped` and recovered heap to 57,435 / dma 19,444.
Low-water marks measured on the two runs: **`minEver=776`** and **`minEver=1,211`**.

**Sharper isolation — the listener is alive; it is the response body that cannot be
built.** "HTTP does not work with the BLE API armed" is too coarse. Measured with BLE
armed, AP up and the WebUI running (free 14,771 / dma largest 4,596):

| Probe | Result |
|---|---|
| ICMP to `172.0.0.1` | 3/3, 0% loss |
| TCP connect to port 80 | **accepted** — SYN/ACK completes |
| `GET /` (a real page body) | connects, sends the request, **0 bytes received** at both 10 s and 20 s |
| `POST /login` (302, no body) | full response, sub-second |
| `POST /cm` (21-byte body) | `command uptime queued`, sub-second |

So AsyncTCP is listening and small replies are fine; the failure is specific to
allocating a real response body. This matches the portal result in ISSUE-21 — small
routes 200, the 4,726-byte page stalled — and means an app that probes reachability
with a small request will conclude the transport is healthy when it cannot serve
anything useful.

**New route added to the pattern, 2026-07-30: `/getscreen` stalls.** With BLE armed,
the AP up and a healthy `webui post-begin` profile (dma largest **6,900**),
`GET /getscreen` returned **`http=000`, 0 bytes, at a 30 s timeout**. It is a large
body by construction — `MAX_LOG_ENTRIES * MAX_LOG_SIZE` of TFT draw log
(`webInterface.cpp:482-500`) — so it belongs with `GET /` rather than with the small
routes. Worth naming because `TEST_STATUS.md` lists `/getscreen` under "All HTTP
routes … 200 and sub-second", which holds **only with `ble api off`**.

**One claim in this entry is contradicted and is corrected here.** It states that
*"After the large request every route went to `http=000`."* That did **not** hold in
this run: immediately after the 30 s `/getscreen` stall, `POST /cm` still returned
**200 in 5–22 ms**, twice. So a large-body stall does not necessarily poison the small
routes — the earlier observation was a lower-heap case, not a general rule.

The stall was not free, though: heap afterwards read free **12,404**, `minEver`
**2,156**, dma largest **2,932** — down from 6,900, and inside ISSUE-25's abort band.

---

### ISSUE-17 — the JS interpreter ~~permanently retains~~ transiently holds ~18 KB of internal heap

**Status:** RESOLVED 2026-07-30 — **not a leak; the title and premise were wrong** ·
**Severity:** was high · **Root cause established, no code change needed**

Running any `js run_from_buffer` script leaves internal heap permanently lower until
the device is rebooted. Measured on the same boot, 2026-07-29:

| Point | free internal | largest block | PSRAM free |
|---|---|---|---|
| fresh boot, BLE armed | 81,391 | 31,732 | 8,382,704 |
| after several `js run_from_buffer` runs | 62,763 | 31,732 | 7,866,164 |
| after reboot | 81,391 | 31,732 | 8,382,704 |

About **18.6 KB of internal heap and ~516 KB of PSRAM** are not returned. A reboot
restores both exactly, so this is retention, not fragmentation.

**Not established:** whether this is a true leak (unbounded, growing per run) or a
one-off interpreter context that is allocated on first use and cached. The measurements
above cannot distinguish those, because several runs were made between the two
readings. Distinguishing them needs `free` sampled after each individual run — the
obvious next experiment, and cheap.

**Why it matters beyond memory accounting:** this is precisely the ~18 KB that makes
the difference between `webui` starting and failing silently (ISSUE-12). An app that
runs a JS payload and then tries to move bulk transfer onto HTTP will find the HTTP
transport gone, with no error from either verb.

---

**RESOLVED 2026-07-30 — it is the interpreter task's stack, and it comes back on its
own in about two seconds. There is no leak and no retention.**

The open question this entry posed ("true leak, or a one-off cached context?") has a
third answer neither option covered.

**Root cause.** `startInterpreterTask()` creates the script task with
`INTERPRETER_TASK_STACK_SIZE`, which is **16384** bytes on a PSRAM board
(`include/precompiler_flags.h:12-13`). With the FreeRTOS TCB that is ~17.8 KB of
*internal* DRAM — matching the measured **17,828 bytes** almost exactly. The task ends
with `vTaskDelete(NULL)` (`interpreter.cpp:114`), and FreeRTOS reclaims a
self-deleted task's stack **in the idle task**, not at the delete call. So the memory
is outstanding only until the idle task next runs.

**Measured, fresh boot, one `js run_from_file` then `free` sampled repeatedly:**

| when | free | largest | dma |
|---|---|---|---|
| immediately after the verb | **62,683** | 31,732 | 31,732 |
| +2 s | **80,511** | 31,732 | 31,732 |
| +4 s / +8 s / +15 s | 80,511 | 31,732 | 31,732 |

Fully recovered within 2 s and flat thereafter. `largest` and `dma largest` never
moved off 31,732 at any point, so it never fragmented either.

**Three claims in the original entry are wrong and are corrected here:**

1. **"until the device is rebooted"** — no. It returns in ~2 s with no reboot.
2. **"~516 KB of PSRAM are not returned"** — not reproducible. Across every
   measurement PSRAM moved by at most **52 bytes** (8,382,704 → 8,382,652 → back).
3. **The `run_from_buffer` vs `run_from_file` distinction claimed mid-investigation
   was a measurement artifact and is retracted.** Six proven-executed
   `run_from_buffer` runs looked flat only because that harness slept 0.8 s and ran a
   `cat` before sampling, giving the idle task time to reclaim; the `run_from_file`
   samples were taken immediately. **Both entry points behave identically.** Recorded
   because it is exactly the trap this register exists to catch — the sampling delay
   was the variable, not the code path.

Every run above was proven to have executed by a filesystem side effect, because `js`
has no return channel (ISSUE-15) and a silent no-op is otherwise indistinguishable
from success. An earlier attempt at this table was meaningless for precisely that
reason: it under-declared the buffer size, the scripts never ran, and the heap was
flat because *nothing happened*.

**But the ISSUE-12 consequence is real, and now has a trivial remedy.** Firing
`webui -bg` *immediately* after a script lands inside the ~2 s window and the WebUI
fails hard — console-captured on ELF `76d42c72f2b4a8a4`:

```
[RAMLOG] stage=webui pre-alloc  heap free= 10,392 largest= 7,156 dma= 2,548
[RAMLOG] stage=webui pre-ws     heap free=  2,000 largest= 1,780 dma=     8
[E][AsyncTCP.cpp:1521] begin(): failed to start task
[RAMLOG] stage=webui post-begin heap free=  1,600 largest= 1,396 dma=     4
```

`webui -bg` still replied `AP`. Externally the AP was on air and ICMP was 3/3, but
`POST /login` and `POST /cm` both returned **`http=000` in ~1.4 ms with curl exit 7**
— TCP connect *refused*, the server never listened.

**Waiting ~3 s first removes the problem entirely.** Same firmware, same script, one
`free` in between: heap back to 80,811, then `webui -bg` reached
`post-begin` at free 15,079 / **dma largest 6,644** — the known-good profile.

**Corrected operational rule:** *let a script finish and give the device ~2 s before
starting the WebUI.* The previous rule — treat any `js` run as permanently poisoning
`webui` until reboot — was far stronger than the evidence, and wrong.

⚠️ **Two distinct HTTP failure modes must not be conflated**, because they look alike
in a one-line curl result and mean opposite things:

| Mode | Signature | Meaning |
|---|---|---|
| Server never started | connect **refused**, `http=000` in ~1 ms, curl exit 7 | AsyncTCP could not allocate its task (this entry, ISSUE-12) |
| Server started, no RAM for a body | connect **accepted**, stalls to timeout, `http=000` | the response body cannot be allocated (ISSUE-16, ISSUE-21) |

---

### ISSUE-18 — `POST /login` writes the whole config to flash, and can abort the device

**Status:** **RESOLVED in `22ab5974`** — sessions are RAM-only and are no longer
serialised at all · **Severity:** was critical (it is the first request any client makes) ·
**Verified** 2026-07-29 · **Crash 1/1 with an ELF-matched backtrace; HTTP failure 2/2**

> **Bookkeeping correction, 2026-07-30.** This entry sat marked OPEN for two sessions
> *after* the fix had already shipped. `22ab5974` removed `saveFile()` from all three
> session functions and its commit body records the hardware result: **8 consecutive
> logins caused no abort where ~4 used to, and latency fell from 350 ms–2.35 s to
> 67–237 ms.** Confirmed against the current tree — `addWebUISession`
> (`config.cpp:870-874`), `removeWebUISession` (`:876-883`) and `isValidWebUISession`
> (`:885-909`) contain no `saveFile()`, and the last carries an explicit comment saying
> why. **Nothing was re-run today; this is a records fix, not a new verification.**
>
> The fix was broader than this entry's title: the write came from **three** call sites,
> and `isValidWebUISession()` runs on *every* authenticated request whose token is not
> already the most recent — so the flash write was reachable from **any** authenticated
> route, not just `POST /login`.

Each successful login appends a session token and immediately persists the **entire**
config file to LittleFS. Under the memory pressure the WebUI itself creates, that
`fopen` cannot allocate and newlib calls `abort()`.

**Decoded backtrace**, innermost last. `ELF file SHA256: b02178b48` matches
`.pio/build/smoochiee-board/firmware.elf` (`b02178b485345ef7`), so this decode is
authoritative:

```
abort() was called at PC 0x40378e97 on core 1

operator()                              webInterface.cpp:435   <- POST /login handler
BruceConfig::addWebUISession            config.cpp:865
BruceConfig::saveFile                   config.cpp:445
fs::FS::open                            FS.cpp:209
VFSImpl::open                           vfs_api.cpp:78
VFSFileImpl::VFSFileImpl                vfs_api.cpp:318
fopen                                   newlib fopen.c:168
__sfp                                   newlib findfp.c:201
__retarget_lock_init_recursive          newlib locks.c:303
lock_init_generic                       newlib locks.c:77      <- abort()
Rebooting... rst:0xc (RTC_SW_CPU_RST)
```

**Observed twice, with different outcomes** — both from a fresh boot with a station
associated:

| Run | Logins before failure | Outcome |
|---|---|---|
| 1 | ~4 (mixed with other requests) | **abort + reboot**, backtrace above |
| 2 | 2 | HTTP stopped answering (`http=000`); **no reboot**, uptime continuous 1:41 → 2:08 |

Memory at the end of run 2: free 12,723, **`minEver=620` bytes**, DMA largest 2,804.
So both outcomes are the same exhaustion; whether it aborts or merely stops serving is
not deterministic.

**Consequences.** Logging in is the first thing any HTTP client does, and the token is
not reusable indefinitely — the app must re-login, and each attempt rewrites the config
file. This also puts avoidable write cycles on flash for what is a session-lifetime
value.

**Auth itself is sound** and was verified separately: `POST /login` with
`admin`/`bruce` returns `302` + `Set-Cookie: BRUCESESSION=…; Path=/; HttpOnly`, a wrong
password returns `302` to `/?failed` with **no** cookie, and `GET /systeminfo` without
credentials returns **401 Unauthorized**.

**Fix direction (not implemented):** keep sessions in RAM, or persist them lazily
rather than on every login; and check the `FS::open` result instead of letting newlib
abort.

**Status update: PARTIALLY FIXED 2026-07-30 in ELF `411d7e151dbc2356`** — the flash
write is gone; the underlying memory ceiling is not.

**The defect was wider than this entry recorded.** `saveFile()` was called from
**three** places, not one: `addWebUISession()`, `removeWebUISession()` **and
`isValidWebUISession()`** (`config.cpp`). The last runs on *every authenticated
request* whose token is not already the most-recent one — so the whole-config flash
write was reachable from any authenticated route, not just `POST /login`.

**Fix.** Session tokens are now RAM-only. All three `saveFile()` calls are removed,
`webUISessions` is no longer serialised by `toJson()`, and any tokens still present
in an older config file are discarded at load rather than restored — they are stale
bearer tokens from a previous boot. Sessions not surviving a reboot is the more
correct behaviour anyway, and it was never worth a flash write.

This also closes part of ISSUE-23: live bearer tokens no longer appear in the
`settings` dump, because they are no longer in the serialised config at all.

**Measured, 8 consecutive logins on a fresh boot:**

| | before (ELF `76d42c72f2b4a8a4`) | after (ELF `411d7e151dbc2356`) |
|---|---|---|
| login latency | 350 ms – 2.35 s | **67 – 237 ms** |
| device survival | **abort + reboot after ~4** | **8 logins, no abort**, uptime continuous |
| `webui post-begin` dma largest | 6,644 – 6,900 | **7,156** (smaller config) |

*What is still wrong:* logins remain **non-deterministic under memory pressure**.
In the 8-login run, #1–#4 and #7 returned 302 + cookie sub-second while #5, #6 and #8
stalled to a 30 s timeout. That is the ISSUE-16 body-allocation ceiling, not this
entry's flash write, and it is unfixed. BLE kept answering `uptime` throughout
(00:02:30, continuous), so the device degraded rather than died — which is the
improvement this fix actually delivers.

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
| HTTP auth (AP mode) | `POST /login admin/bruce` → 302 + `Set-Cookie: BRUCESESSION=…; Path=/; HttpOnly`. Wrong password → 302 to `/?failed`, **no** cookie. `GET /systeminfo` unauthenticated → **401**. 2026-07-29. |
| AP addressing is **172.0.0.1**, not 192.168.4.1 | Laptop associated and got `172.0.0.3/24`, default via `172.0.0.1`. `GET /` → 200, 601 bytes. Contract §said 192.168.4.1; that is the *Evil Portal* gateway override, not the WebUI AP. |
| `reboot` verb | Link drops mid-write, device returns in ~12 s with the BLE API re-armed; console shows `rst:0xc (RTC_SW_CPU_RST)`. 2/2. |
| Full file CRUD over BLE | `mkdir`, `storage write` (+EOF/5 s line mode), `cat`, `md5`, `crc32`, `storage stat`, `storage copy`, `storage rename`, `rm`, `rmdir` — all exercised and the filesystem left byte-identical to session start. MD5 of a copy matched the source. |
| `gpio` read/mode/set | On `smoochiee-board`, `is_free_gpio_pin` allows **only 47/48** (`gpio_commands.cpp:4-26`). `gpio mode 47 3` (OUTPUT, `0x03`) then `set 1` → reads 1, `set 0` → reads 0. Invalid pin and invalid value both rejected cleanly. **Success returns an empty reply.** |
| Missing-module verbs fail cleanly | `nrf24`, `gps`, `getscreen` → `ERROR: Command not found` (no `nrf_commands.cpp`/`gps_commands.cpp` exist). `rfid info` → "No tag data…", `rfid reset` → "No active RFID module." `ir rx` bounded at ~9.1 s. |
| `js` executes correctly | Proven by side effect: `storage.write("/js_ran.txt","JS_EXECUTED")` then `cat` returned the content. Output/errors are a separate defect (ISSUE-15). |
| Heap fully recovers on reboot | After WebUI + JS runs left free at 44,255 / largest 11,252, a reboot restored 81,391 / 31,732 exactly, and `ls /` returned to a complete 297 bytes. |
| **HTTP `nav` rescue releases a blocked verb** | `ap_info` blocked (BLE silent), then `POST /cm cmnd=nav sel` ×6 at 1 s intervals released it — `uptime` answered in 152 ms, uptime continuous 2:00→6:54→7:25, zero crash markers. **One pulse is not enough** — see ISSUE-19. |
| HTTP survives a non-radio blocking verb | With `ap_info` holding the serial task, `POST /cm cmnd=uptime` still returned `command uptime queued` / 200. The AsyncWebServer task is genuinely independent of the CLI queue. |
| `POST /cm` auth + queueing | Cookie auth accepted; returns `command <verb> queued` and never the output, as documented. |
| Menu exit paths | `loopOptions` breaks on `check(EscPress)` for non-main menus (`display.cpp:647`); `addOptionToMainMenu()` also pushes a `Main Menu` option calling `backToMenu()` (`utils.cpp:27-30`). Either works. |
| **Headless portal is genuinely headless** | `uptime` over BLE answered in **0.06 s** during a live background portal, where the blocking verb held the serial task for its entire life. `evilportal -status` answered 9 consecutive times during a running portal. ELF `76d42c72f2b4a8a4`, 2026-07-30. |
| Portal duration cap self-stops | 45 s cap fired at **+45.6 s**; events in order `portal duration cap reached` → `state:idle` → `portal stopped`. This is the only recovery path that survives `ble api off`. |
| Cap path writes nothing to the CLI | **Zero** unsolicited bytes on the CLI characteristic across a full cap firing; `uptime` immediately after returned a clean `Uptime: 00:01:35`. Confirms `stopPortal(announceOnCli=false)` does not touch `serialDevice` — the design's central framing claim. |
| AP genuinely on air, independently checked | `nmcli` scan showed `PortalTest`, ch 6, signal 90, open — not taken from the device's own reply. Laptop associated and got `172.0.0.2/24`. |
| BLE + AP + WebUI + HTTP **do** coexist | 2026-07-30: with the BLE API armed, `POST /login` returned 302 + cookie in 1.33 s and `POST /cm` returned `command uptime queued`, while BLE `uptime` kept answering. Small responses only — see ISSUE-16 for what does not work. |
| `nav` is handled before queueing | Reply is `command nav esc success`, distinct from `command <verb> queued` — so the AsyncWebServer task writes the button globals directly and never reaches the serial queue (`webInterface.cpp:537-552`). Previously a code-only claim. |
| Reset-cause calibration | RST button → `rst:0x1 (POWERON)`; software `reboot` verb → `rst:0xc (RTC_SW_CPU_RST)`. Distinguishable in any console capture, which is how an operator reset was told apart from a firmware fault. 2026-07-30. |
| `POST /login` form fields | `username` / `password` (`webInterface.cpp:425-426`). Posting `user`/`pwd` returns `302 Location: /?failed` — **indistinguishable from a wrong password**. |

---

## Open — later findings

Entries from 2026-07-29 onward. These were appended after the tables above and had been
sitting under §Verified working with no heading of their own, which made 22 open defects
invisible to anyone scanning §Open. Same status rules as §Open; kept in a second section
only to preserve the numbering and the existing anchors.

---

### ISSUE-19 — the HTTP `nav` rescue works, but needs repeated pulses and is unavailable for every radio verb

**Status:** OPEN · **Severity:** high (it is the only remote recovery path there is) ·
**Verified** 2026-07-29 · **Rescue confirmed on `ap_info`; both blockers reproduced**

`POST /cm cmnd=nav <button>` **does** release a blocked verb — this was previously
code-verified only. But two things make it far narrower than the contract implied.

**1. A single pulse is not enough.** `ap_info` was dispatched over BLE and confirmed
blocking (no reply to it, nor to a follow-up `uptime`). Then, all accepted `http=200`:

| Attempt | Result |
|---|---|
| `cmnd=nav sel` (one pulse) | still blocked |
| `cmnd=nav sel 1000` ×2, then `cmnd=nav esc 1000` | still blocked |
| `cmnd=nav sel` ×6, 1 s apart | **released** — `uptime` answered in 152 ms |

Uptime was continuous across the whole sequence (2:00 → 6:54 → 7:25) so this was a
genuine release, not a reboot, and the capture contains **zero** crash markers.

Two properties of the code explain why one pulse fails. The handler only holds the
button for **10 ms** unless the command *ends in a `0`* —
`if (cmnd.endsWith("0")) time = …; else time = 10;` (`webInterface.cpp:546-548`) — so
`nav sel` is a single ~190 ms pulse. And the waiter takes two edges, not one:

```cpp
// scrollableTextArea.cpp:73-83
while (check(SelPress))  { update(force); yield(); }   // wait for RELEASE
while (!check(SelPress)) { update(force); yield(); }   // then wait for PRESS
```

A single held pulse can be consumed entirely by the release-wait, leaving the
press-wait unsatisfied. Repeated pulses *with gaps* supply both edges.

**The exact minimum was not measured** — 1 failed and 6 worked, with intermediate
attempts in between. Treat "repeat until the device answers" as the contract, not a
fixed count.

**Re-measured 2026-07-30 on ELF `411d7e151dbc2356`, and the answer is that there is no
fixed minimum.** Same verb, same rescue, single pulses 3 s apart against one blocked
`ap_info`: pulse **#1 failed** (still blocked 25 s later), **#2 failed**, **#3 failed**,
**#4 released it** — `{"type":"attack_result","verb":"ap_info","outcome":"completed",
"elapsed_ms":109994,...}` then the queued `uptime` answered. Against 2026-07-29's
"1 failed, 6 worked", the count is **nondeterministic**. This closes the "minimum pulse
count" row in §Not tested with the answer *there isn't one* — do not go looking for a
threshold to bisect.

**The two-edges explanation above is incomplete; the real mechanism is a three-way race.**
Because `check()` **clears the flag as it reads it** (`include/globals.h:244-283`),
whichever of the two loops happens to be active *consumes* the pulse:

- land while `while (check(SelPress))` is spinning → the pulse is eaten by the
  release-wait, that loop exits, and the press-wait then waits for a *fresh* press;
- land while `while (!check(SelPress))` is spinning → one pulse is enough.

Racing both is a third party the earlier analysis missed: **`taskInputHandler`
(`src/main.cpp:84-111`) sets every button global back to false every ≤75 ms**, so it can
erase a pulse before *either* loop observes it. Meanwhile the handler holds the flag high
for just one 190 ms iteration. Three tasks, no synchronisation — hence a count that
varies run to run rather than a fixed two.

**Consequence for the app is unchanged but now rests on a mechanism rather than a guess:**
pulse until the device answers, and never present the rescue as taking a known number of
presses.

**2. The rescue is unavailable for every verb that touches a radio.** Two independent
mechanisms destroy the HTTP transport before it can be used:

- **WiFi verbs kill the WebUI on entry.** `cleanlyStopWebUiForWiFiFeature()` is called
  by `deauthFloodAttack` (`wifi_atks.cpp:369`), `capture_handshake` (`:447`),
  `target_atk` (`:732`), `karma` (`karma_attack.cpp:2559`), `evilportal`
  (`evil_portal.cpp:29`) and `sniffer` (`sniffer.cpp:1139`).
- **BLE verbs destroy WiFi.** Reproduced live: `blesniffer` dispatched with the WebUI
  up and a station associated tore the WiFi stack down —
  `wifi_init_default: netstack cb reg failed with 12308` and
  `disconnect(): STA disconnect failed! 0x3001: ESP_ERR_WIFI_NOT_INIT`. The AP
  dropped, the laptop disconnected, and `POST /cm` could no longer be delivered.

  **Mechanism SUSPECTED, not verified.** The only matching code path is
  `radioHasMemForBle()` (`BLE_Suite.cpp:313`), whose precondition is measured — the
  largest DMA block with the WebUI up was **7,156**, under the 15,360
  `RADIO_BLE_MIN_DMA_BLOCK` threshold (`radio_mem.h:32`) — and whose fallback calls
  `wifiDisconnect()`. But **neither** its `[RAM] Low contiguous DMA memory…` line nor
  `displayError("Low RAM: free WiFi/SD first")` appears in the capture. The other
  candidate, the explicit `wifiDisconnect()` at `BLE_Suite.cpp:302-311`, is **ruled
  out**: it is gated on `FORCE_RADIO_TEARDOWN_ON_SWITCH`, which is `false` here
  because the PSRAM-conditional around it is disabled by an `#if 0`
  (`ble_common.h:32-40`). The USB console has demonstrably dropped output under
  memory pressure this session, so the missing log line is weak evidence either way.

**So the rescue only works for blocking verbs that touch neither radio** — `ap_info`
and possibly `pwngrid`. For `deauth`, `karma`, `evilportal`, `sniffer` and
`blesniffer` there is **no remote recovery path at all**, over either transport.

**Consequence for the app.** Offer the rescue only for the verbs it can actually
serve, and pulse until the device answers rather than firing once. For the radio
verbs, the honest UI is to warn before dispatch that the action can only be ended at
the device — not to offer a cancel button that cannot work.

---

### ISSUE-20 — `badusb` cannot work on this build and hangs the device forever

**Status:** PARTIALLY FIXED in `b1c825c8` (hang gone; BadUSB still cannot type) · **Severity:** critical (unrecoverable hang; and it retires a
capability previously believed shippable) · **Verified** 2026-07-29 ·
**Root cause fully code-verified**

`badusb run_from_file` types **nothing** and **never returns**. The serial task is
held indefinitely — no reply to the verb after 45 s, none to a follow-up `uptime`, and
no error on any channel. Only a physical reset recovers it.

**Observed** (user watching a focused text editor, 2026-07-29): payload
`DELAY 3000` + `STRING BRUCELINK_BADUSB_OK_20260729`, written to `/bl_ducky.txt` and
verified by `cat`. Nothing was typed. BLE remained connectable throughout, so the
device was alive — only the CLI task was stuck.

**Root cause — TinyUSB is not the active USB stack on this board.**

```cpp
// ducky_typer.cpp, ducky_startKb(), the #if defined(USB_as_HID) branch
hid = new USBHIDKeyboard();
USB.begin();
while (!tud_mounted()) {                  // <- unbounded, no timeout, no escape
    printStatusBadUSBBLE("Waiting USB Host...");
    delay(500);
}
```

`tud_mounted()` can never become true here, because the board is compiled for the
hardware USB Serial/JTAG peripheral rather than TinyUSB:

```
boards/_boards_json/smoochiee-board.json → build.extra_flags:
  -DARDUINO_USB_CDC_ON_BOOT  -DARDUINO_USB_MODE=1
```

and the framework gates the two stacks against exactly that flag:

| Framework source | Meaning |
|---|---|
| `HardwareSerial.h:440` | `#if ARDUINO_USB_MODE` → *Hardware CDC mode*; `#else` → *Native USB Mode* |
| `main.cpp:6` | TinyUSB is initialised only `#if (…ON_BOOT) && !ARDUINO_USB_MODE` |
| `HWCDC.h:109` | `ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT` → *Hardware JTAG CDC selected* |

So `ARDUINO_USB_MODE=1` selects hardware JTAG CDC and leaves TinyUSB uninitialised,
while `USB_as_HID` is defined regardless (`boards/smoochiee-board/pins_arduino.h:25`)
— so the HID branch is compiled in and then waits forever for a stack that was never
started.

**This is the compile-time-flag trap biting for real.** `USB_as_HID` being defined is
what made BadUSB look like the one attack class needing no extra hardware. It is the
same class of error as the `/systeminfo` capability flags (ISSUE-4), except here the
consequence is a hang rather than a wrong label.

**Corrects an earlier claim.** Prior notes recorded BadUSB as "fitted hardware,
registered and unexposed by the app", and the readiness brief listed
`badusb run_from_file` / `run_from_buffer` as shippable and needing no extra hardware.
**That is false on this build.** Being `#define`d is not being usable.

**Both entry points are affected.** `badusbBufferCallback` reaches the same
`ducky_startKb(hid_usb, false)`, so `run_from_buffer` hangs identically. The BLE-HID
path (`ducky_startKb(..., ble=true)`) is a **different** branch that does not touch
TinyUSB — it builds a `BleKeyboard` — and was **not tested**. It may well work, and is
the more interesting option anyway since it needs no cable to the target.

**Fix directions (none implemented), in increasing order of cost:**

1. **Bound the wait.** `while (!tud_mounted())` should time out and report, instead of
   hanging the only command surface the app has. This is worth doing regardless of
   which USB mode the board ships, since it converts an unrecoverable hang into an
   error the app can render.
2. **Refuse early.** Gate the USB branch on the USB mode actually compiled
   (`#if ARDUINO_USB_MODE == 1` → return a clear "USB HID unavailable in hardware-CDC
   mode" rather than attempting it).
3. **Build with `ARDUINO_USB_MODE=0`** to get real TinyUSB HID. **This has a real
   cost**: the hardware JTAG CDC console is how panic backtraces are captured on this
   board (see ISSUE-1's decode), so changing it affects the primary debugging channel.
   Not a change to make casually.

**Partial fix (`b1c825c8`).** The wait is now bounded at
`USB_HID_MOUNT_TIMEOUT_MS` (8 s). On timeout the handle is deleted and nulled, and
`key_input()` gained a single null guard covering all nine `ducky_startKb()` call
sites rather than a check at each.

*What this fixed:* the unrecoverable hang. Verified 2026-07-29 on ELF
`4bdcd1dc364fd2cf` — `badusb run_from_file bl_ducky.txt` returns in **9.3 s** with
`eot=True` and the device stays responsive, where it previously spun forever and took
the serial task, the app's only command surface, with it. The `TX Semaphore is NULL`
flood that the first attempt at this fix exposed went from **60 occurrences to 0**.

The console now says why, which it could not before — the original diagnostic used
`Serial.println`, and on a board built with `ARDUINO_USB_CDC_ON_BOOT` that does not
reach the port carrying the ESP-IDF console (the same trap as ISSUE-2). Switched to
`log_e`:

```
[E][ducky_typer.cpp:683] ducky_startKb(): USB host did not enumerate in 8000 ms - is TinyUSB active on this build?
[E][ducky_typer.cpp:846] key_input(): no HID interface - keyboard was never started
```

**This also upgrades the root cause from code-reading to observation:** the timeout
branch fires, so `tud_mounted()` genuinely never becomes true on this build.

*What is still wrong:* **BadUSB over USB HID still cannot type anything.** That needs
`ARDUINO_USB_MODE=0`, which would give up the hardware JTAG CDC console this board
relies on — a board-configuration trade-off, not a bug fix. The BLE-HID branch
(`ducky_startKb(..., ble=true)`) is a different path that never touches TinyUSB and
remains **untested**.

---

### ISSUE-21 — Evil Portal cannot serve its own page on this board

**Status:** **RESOLVED 2026-07-30 in `evil_portal.{h,cpp}`** — the response died on the
fourth TCP segment and dropped exactly the login form; fixed by streaming the page from
`.rodata`. **Verified 8/8 on hardware**, ELF `e81b0c28f80e70dd` · **Severity:** was high
(the attack was inert — no victim ever saw a form)

> This entry was twice resolved on a wrong reading before the real cause was found —
> first as "the memory ceiling, not a portal defect" (2026-07-29), then quantified as
> "~13 KB per load, no second load" (2026-07-30). Both are left in place below, marked,
> because the corrections are the useful part. Read §Root cause for what is actually true.

The portal starts, advertises, and hands out DHCP leases, but does not serve a usable
page. The AP works; the HTTP responder does not.

**Observed**, `evilportal BL_PORTAL_TEST 6` with the BLE API armed:

- The AP appears as an **open** network (`softAP(apName, emptyString, _channel)`,
  `evil_portal.cpp:136`) and DHCP works — a laptop got `172.0.0.4/24`, gateway
  `172.0.0.1`.
- A phone loaded the portal and got an **incomplete page**: logo area present, **both
  form fields absent**. A complete default page is **4,726 bytes**
  (`loadDefaultHtml()`, `evil_portal.cpp:599-655`) and the `email`/`password` inputs
  live in its last ~600 bytes — exactly where a truncated response would cut off.
- From a laptop, with the phone disconnected and **only one client associated**, the
  gateway answered **neither HTTP nor ICMP**: 7 `curl` attempts across two sessions
  all returned `http=000`, and `ping` reported 100% loss.
- The console was **silent throughout** — no allocation error, no warning.

**Consequence:** no credential can be captured, because no victim is ever shown a
form. `md5 /BruceEvilCreds/_creds.csv` was **unchanged** (`f42bfe126ee98b466e4349e666fadd4c`)
before and after the run, confirming nothing was written.

**Cause SUSPECTED, not established: memory.** It is consistent with everything else
measured on this board — the WebUI needs ~18 KB of margin and fails silently below it
(ISSUE-12), and a 4,726-byte response plus AsyncTCP buffers is a large ask when the
BLE API is holding ~62 KB. But **the free-heap number that would prove it was never
captured**, because `evilportal` blocks the CLI for its entire life and the device
crashed (ISSUE-1) before it could be released. Do not record this as proven.

**Distinguishing experiment:** run `evilportal` with the BLE API disarmed and measure
`free` from the USB console, or add a RAMLOG stage inside `portalController`. Either
would separate "not enough heap for the response" from "the responder is broken".

**Telemetry gap found alongside.** `recordPageView()` is called on every portal hit
but never emits an event — verified in code and on the wire, with the event
characteristic subscribed for 542 s. The only frame the whole run produced was
`{"id":12,"type":"state","device_state":"portal"}` at +2.72 s. **The app cannot tell
that anyone loaded the portal, nor that a credential was captured.**

**The missing measurement, obtained 2026-07-29.** This entry recorded that free heap
during an active portal had never been captured, because the verb blocks the CLI for
its whole life. It was captured accidentally during an unrelated test: across one
portal run with two clients associating, `minEver` fell from **31,559 to 163 bytes**.

That single number accounts for every symptom at once — no page served, a second
client unable to associate, and the portal exiting unprompted. **The cause is memory
exhaustion, not a routing or handler defect**, which makes fixing it a different piece
of work than previously scoped.

Consistent with the transport finding in the contract §1: with the BLE API armed there
is not enough heap to serve HTTP content at all. With `ble api off` (121,247 free)
every HTTP route returns 200 sub-second. **The portal has never been tested with BLE
off** — that is the obvious next experiment and may make it work as designed.

Also observed and **unexplained**: the portal exited with nobody touching the device,
contradicting the documented "no timeout, exits only via Esc → Exit Portal". Likely an
allocation failure down some path, but that is a guess and is not recorded as fact.

**RESOLVED 2026-07-29 — the portal was never broken.** Run with `ble api off`
(121,247 free instead of 15,040) the entire flow works, end to end, on ELF
`5186685c0fdf19c2`:

| Request | Status | Size | Time |
|---|---|---|---|
| `GET /` | 200 | 4,726 B | 0.01 s — the "Sign in: Google Accounts" page |
| `GET /generate_204` | 200 | 4,726 B | 0.24 s — Android captive-portal probe |
| `GET /hotspot-detect.html` | 200 | 99 B | 0.01 s — iOS probe, redirects to the portal |
| `GET`/`POST /post` | 200 | 2,357 B | 0.20 s — credential submission accepted |
| `GET /creds` | 200 | 2,431 B | — **captured credentials returned** |

**Credential capture verified**, which this register had listed as untested *and*
blocked by this very entry:

```
email: testvictim@example.com
password: NotARealPassword123
```

Details worth keeping: the form is `<form action='/post' id='login-form'>` with fields
**`email`** and **`password`**, and it carries no `method=`, so a browser issues a GET
— both GET and POST are accepted and both were captured. The retrieval endpoint
defaults to **`/creds`** (`config.cpp:689`); `/BruceEvilCreds` is a filesystem path,
not a route, and returns the portal page.

**So this was never a routing or handler defect.** It is the same memory ceiling
behind ISSUE-12 and ISSUE-16 — with the BLE API armed there is not enough heap to
serve any HTTP body at all (contract §1).

⚠️ **Operational cost, and it is severe.** `evilportal` calls
`cleanlyStopWebUiForWiFiFeature()` on entry, destroying the WebUI and its AP. Dispatch
it with BLE already off and there is **no remote control surface left at all** — no
BLE, no WebUI, and no serial CLI exists (ISSUE-22). The portal serves no exit route
(`/clear` only wipes captured passwords). Recovery is on-device only: Esc chord
(LEFT+RIGHT) → "Exit Portal", then Config → BLE API toggle, because `ble api off`
persisted `bleApiAutoStart = 0` and even a reboot comes back without BLE.

**An app must not offer this combination unless the operator is at the device.** It
becomes safe once `evilportal` gains a headless, remotely stoppable entry point
(§5.3, still open).

**UPDATE 2026-07-30 — the headless entry point now exists, and the starvation
reproduced with numbers attached.** `evilportal -bg` shipped on this branch (see
`FIRMWARE_CHANGES.md`), and the load-bearing claim was proven on hardware: `uptime`
over BLE answered in **0.06 s** during a live portal, where the blocking verb had
previously held the serial task for its entire life. `evilportal -status` answered 9
consecutive times during a running portal.

**But the memory ceiling in this entry is still there, and it still prevents credential
capture with the BLE API armed.** Measured on ELF `76d42c72f2b4a8a4`:

| | with BLE armed | idle reference |
|---|---|---|
| free heap at portal start | **16,915** | 80,951 |
| dma largest at portal start | **8,180** | 31,732 |

`GET /` delivered **2,766 of the expected 4,726 bytes and then stalled** until timeout.
The `email` and `password` inputs live in the last ~600 bytes and never arrived — which
reproduces this entry's original symptom exactly, from the opposite direction. Small
routes were unaffected: `/hotspot-detect.html` returned 200/99 B in 0.005 s and
`/generate_204` returned 302. After the large request every route went to `http=000`.

So the resolution above stands — the portal is not broken — but **the configuration in
which it works is still `ble api off` and nothing has changed that.** The headless verb
removes the *stranding* risk (the duration cap self-stops the portal, verified at
+45.6 s on a 45 s cap) without removing the *memory* constraint.

**Credential capture with `ble api off` remains the only proven-working configuration,
and it has not been re-run since the headless work landed.**

---

**Quantified 2026-07-30 on ELF `411d7e151dbc2356` — the mechanism is now a number, and
the ceiling is one page load.**

Measured against a background portal with the BLE API armed (start:
`free_heap:17207 dma_block:6132`), from a laptop on the portal AP:

| Request | Result |
|---|---|
| `GET /` #1 | **200, 4,726 bytes, 0.33 s** — the full page, correctly |
| `GET /` #2 | `http=000`, 0 bytes, 12 s timeout |
| `GET /` #3 | `http=000`, 0 bytes, 12 s timeout |
| `GET /` #4 | `http=000`, 0 bytes, 12 s timeout |
| `/hotspot-detect.html` after | `http=000` in **0.196 s** — now refused outright, not stalled |
| heap after | **free 4,287 / dma largest 884** |

**⚠️ The conclusion drawn here — "serving the portal page exactly once costs ~13 KB and
leaves the portal unable to serve anything again" — is WRONG, and was refuted on hardware
2026-07-30. See §Root cause below.** The reading was taken only *after* four further failed
requests, so three stalled connections were being charged to the page load. Sampled
immediately after a single `GET /`, the cost is fully returned. The measurements in the
table above stand; the inference from them does not. Note the two distinct failure shapes
in one sequence — the stalls on `/` (connection accepted, body never allocated) and then
the fast refusal on a small route (server no longer accepting) — which is the boundary this
register warns against conflating.

**This is what real handsets hit.** During the ISSUE-27 test both an iPhone and an Android
auto-opened the portal and then showed a blank/failed page: the platform's own probe
request spends part of the budget, and the page request that follows lands after it is
gone. The earlier "2,766 of 4,726 bytes then stalled" observation is the same effect
caught mid-transfer. **So the entry's symptom is not a routing or template defect and not
specific to `ble api off` — it is a per-page-load cost against a ~17 KB budget.**

The memory is **returned when the portal stops** (`free_heap:53143` after `evilportal -off`,
matching the normal plateau), so it accumulates for the portal's lifetime rather than
leaking permanently. `minEver` reached **32 bytes** during these runs, and the console
logged `E BLE_INIT: Malloc failed` twice.

**Separately measured — a portal start/stop cycle has a one-off cost, not a cumulative
one.** Three cycles over a single BLE connection with a fixed 25 s post-stop settle
(equal sampling delay, per this register's own ISSUE-17 lesson):

| | free | dma largest | blocks |
|---|---|---|---|
| pristine boot | 81,091 | 31,732 | 10 |
| after cycle 1 | 53,139 | 18,420 | 19 |
| after cycle 2 | 52,991 | 18,420 | 20 |
| after cycle 3 | 52,991 | 18,420 | 20 |

The first cycle costs ~28 KB and 13 KB of the largest DMA block; later cycles are flat
(−148 bytes total). Three spaced samples were byte-identical, so this is not a sampling
artifact. **Operational consequence: after any portal cycle the largest DMA block sits at
18,420 — only ~3 KB above `RADIO_BLE_MIN_DMA_BLOCK` (15,360).** Portal start reproducibly
costs ~36 KB and takes 3.09 s, the 3,000 ms `yield()` wait at `evil_portal.cpp:170`
dominating (3088/3086/3088/3151 ms across runs).

---

**RESOLVED 2026-07-30 — root cause found, fixed, and verified 8/8 on hardware.**
Fixed in `evil_portal.{h,cpp}`; proven on ELF `e81b0c28f80e70dd`, device
`1C:DB:D4:5E:D7:39`.

**Two claims this entry carried are refuted.**

1. *"The cost is ~13 KB retained."* It is **fully transient**. Sampled around one `GET /`
   with nothing else in flight: `free` 16,307 before → **16,275** immediately after →
   **16,307** at +10 s → **16,307** at +25 s. The peak is real but returns; it is visible
   only in `minEver`, which fell to **344 bytes** during the load.
2. *"There is no second load."* There is. From a pristine boot with a 60 s per-request
   budget: #1 truncated, #2 fail, #3 fail, **#4 = 200 / 4,726 B in 0.193 s**, #5 fail,
   **final = 200 / 4,726 B in 0.372 s**. The failure is **per-request nondeterminism**, not
   a one-shot budget. Two requests from an identical heap state (16,307 free, 8,180 largest)
   gave opposite outcomes.

**Root cause — the response dies on a TCP segment boundary, and the arithmetic is exact.**

A stalled response stops at **exactly 4,202 body bytes**, observed **four independent
times** across three runs and two builds. The response headers were captured on the wire
and are **exactly 106 bytes**:

```
HTTP/1.1 200 OK\r\nConnection: close\r\nAccept-Ranges: none\r\n
Content-Length: 4726\r\nContent-Type: text/html\r\n\r\n
```

`106 + 4,202 = 4,308 = exactly 3 × CONFIG_LWIP_TCP_MSS (1,436)`. **Three TCP segments
allocate; the fourth never does.** `AsyncBasicResponse` hands lwIP the whole body in a
single `client()->write()` (`WebResponses.cpp:308`), and lwIP TX pbufs must be internal
DMA-capable RAM — `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is **not set**. There is no
retry that ever recovers, and **the console prints nothing at all** (two lines across an
entire run, neither related).

**What the victim actually sees, and why the original 2026-07-29 report was right.**
The missing tail is **exactly 524 bytes** (4,726 − 4,202), and it is precisely the
`containersubtitle` plus the entire `<form>`. A captured truncated body ends mid-element
at `<div class='containertitle'>Sign in`, and `grep -c "name='email'"` returns **0**. That
is byte-for-byte this entry's original symptom — *"logo area present, both form fields
absent"*. **The portal was reliably serving everything except the login form.** "Blank
page" understated it: the page rendered, the form was never delivered.

**The fix** (`evil_portal.h`, `evil_portal.cpp:574,609,679` — 25 insertions, 5 deletions):
the two built-in pages become function-local `static const char[]` (`.rodata`, literal text
untouched so the diff stays mergeable), `String htmlPage` becomes
`const char *_defaultHtml` + `_defaultHtmlLen`, and `portalController()` serves them with
`beginResponse(200, "text/html", (const uint8_t *)_defaultHtml, _defaultHtmlLen)` →
`AsyncProgmemResponse`, which writes `min(2872, tcp_win)` per slice and **resumes on the
next ACK** when the window closes (`WebResponses.cpp:428-437`).

**Static storage was chosen for lifetime, not just cost.** `evilPortalBgStop()` does
`delete bgPortal` (`evil_portal_bg.cpp:33-35`), `AsyncWebServer::end()` only calls
`_server.end()` (`WebServer.cpp:116-118`), and `AsyncProgmemResponse::_sourceValid()`
returns `true` unconditionally. A pointer into a member `String` would therefore have been
a use-after-free on a response still in flight at portal stop. A literal cannot be.

**⚠️ The fix works, but not for the reason first predicted — and the measurement is what
caught it.** Removing the body copy was expected to gain ~4.7 KB of internal DRAM at portal
start. It gained **~0** (17,651 vs 17,607/17,683 before). Cause:
**`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`** with `CONFIG_SPIRAM_USE_MALLOC=y`, so *every
allocation over 4 KB lands in PSRAM*. Both the 4,736-byte `htmlPage` String and
`AsyncBasicResponse`'s copy were in PSRAM all along and never competed for internal DRAM.
**The win comes entirely from the bounded per-write pbuf demand and the ACK-driven retry
loop**, not from removing a copy. *Any heap argument on this board that ignores the 4 KB
PSRAM threshold will mis-attribute its cause.*

**Verification — same 8-load harness, same pristine start, same afternoon:**

| build | result |
|---|---|
| `f5244eb35dd10795` (before) | **0/8** — #1 truncated at 4,202 B, #2–#8 `http=000` |
| `e81b0c28f80e70dd` (after) | **8/8 full 4,726 B**, 0.134–0.376 s each |

All eight bodies were **byte-identical** (single md5 `106673d595bbfb149ae056408ecc8bbc`)
and every one contained `name='email'`, `name='password'` and `</html>`.

**Residual, quantified rather than hidden.** Heap drifts 16,567 → 14,303 across eight
back-to-back loads (**~283 B per load**), and the post-stop plateau read **51,387**, about
1.6 KB below the documented 52,231–53,143. **This is not a regression from the fix** — a
following no-load portal cycle returned it to **53,511**. It is per-load residual held past
portal stop and reclaimed on the next cycle; earlier builds never completed eight loads, so
they never accumulated it. Not believed to matter operationally, but it is unexplained.

**Still true after the fix:** the portal leaves only ~16 KB free, so this remains a thin
margin, and `minEver` still reaches ~1,864 under load. The ceiling was not raised — the
response simply no longer demands more than the margin can serve at once.

---

### ISSUE-22 — there is no serial CLI on this board; BLE is the only command interface

**Status:** OPEN · **Severity:** high (determines the recovery story) · **Verified** 2026-07-29

`USBserial` wraps the Arduino `Serial` object, and on this board `Serial` does **not**
reach `/dev/ttyACM0`. Tested with the BLE API off, so nothing else owned
`serialDevice`: `uptime` and `free` were written to the port with CRLF, with LF, and
with a bare newline, and **zero bytes** came back in every case.

The ESP-IDF console output this project has relied on all day — panics, backtraces,
`RAMLOG` markers, `log_e` — arrives on a **separate channel**. Reading that port is
not the same as talking to the CLI.

**Consequences.**

- **BLE is the only remote command interface that exists.** With `ble api off` and no
  WebUI running, the device has **no** remote control surface; recovery is the
  on-device Config menu.
- The app must never drop BLE without first starting the WebUI (see the contract §1
  switch workflow), or it strands the device.
- This is the real mechanism behind **ISSUE-2**: `settings` wrote the config to
  `Serial`, which reaches nobody, on *any* transport.
- It is also why a `Serial.println` diagnostic added while fixing ISSUE-20 was
  invisible until it was changed to `log_e`. **Firmware diagnostics on this board must
  use `log_e`/ESP_LOG, never `Serial.println`.**

**Recorded as a corrected prediction:** it was expected that turning the BLE API off
would hand the CLI back to USB, because `bleApi.end()` sets
`serialDevice = &USBserial` (`ble_api.cpp:110`). It does — but that object is not
connected to anything reachable.

---

### ISSUE-23 — WiFi credentials are stored in plaintext and readable over an unauthenticated link

**Status:** MITIGATED 2026-07-30 (disclosure closed; storage still plaintext, link still unauthenticated) ·
**Severity:** high (secret disclosure) · **Verified** 2026-07-29

`wifi add <ssid> <pwd>` stores the password verbatim and writes it to flash
(`config.cpp:659-662`):

```cpp
void BruceConfig::addWifiCredential(const String &ssid, const String &pwd) {
    wifi[ssid] = pwd;   // plaintext
    saveFile();
}
```

`settings` with no arguments returns that whole config. Demonstrated on hardware with
a deliberately fake credential:

```json
"wifi": { "TESTNET_FAKE": "notarealpassword" }
```

The same dump also carries `webUI: {"user": "admin", "pwd": "bruce"}` and the active
`webUISessions` tokens.

**The BLE command bus has no authentication of any kind.** Any client in range that
connects can read all of it.

**This exposure was widened by the ISSUE-2 fix in `b1c825c8`, deliberately and with
eyes open.** Before it, the dump went to `Serial` and reached nobody (ISSUE-22), so
the secrets were unreachable by accident rather than by design. Making `settings`
work — which the app needs — made them readable. The fix is not the defect; the
absent authentication is.

**Mitigations, none implemented:** redact secret-bearing fields from the `settings`
dump; or require pairing/bonding on the GATT service; or keep credentials out of the
serialised config. Until then, **do not store real network credentials on this
device**, and treat anything in `bruce.conf` as public to anyone within BLE range.

**Mitigation 1 of 3 applied 2026-07-30, ELF `411d7e151dbc2356` — the dump is
redacted.** `settings` and single-field reads now replace every secret-bearing value
with `<redacted>`:

```
settings wifiAp           ->  wifiAp = {"ssid":"BruceNet","pwd":"<redacted>"}
settings webUI            ->  webUI = {"user":"admin","pwd":"<redacted>"}
settings wigleBasicToken  ->  wigleBasicToken = <redacted>
settings                  ->  "wifi": { "TESTNET_FAKE": "<redacted>" }, ...
```

Covers `webUI.pwd`, `wifiAp.pwd`, every value in the `wifi` SSID→password map,
`wigleBasicToken` and `wdgwarsApiKey`. SSIDs and the WebUI username stay visible —
they are operationally useful and not secrets. Session tokens are gone from the dump
entirely, because ISSUE-18's fix stopped serialising them at all.

**Redaction is applied only to the copy the CLI is about to print.** `bruceConfig`
and the file written by `saveFile()` are untouched — redacting inside `toJson()`
would have persisted the placeholder and destroyed the real credentials on the next
save. The single-field path is redacted too, or `settings wifi` would walk straight
around the dump.

*What is still wrong, and why this is MITIGATED rather than RESOLVED:*

- Credentials are **still stored in plaintext** in `bruce.conf` on LittleFS. Anything
  that can read the file — `cat /bruce.conf`, the WebUI file routes — still gets them.
  **This is the bigger hole and it is not closed.**
- The **BLE command bus still has no authentication**. Redaction narrows what a
  drive-by client learns from one verb; it does not make the link trustworthy.
- Guidance is unchanged: **do not store real network credentials on this device.**

---

### ISSUE-24 — a verb dispatched over HTTP `/cm` never repaints the screen

**Status:** RESOLVED — fix `d71f19e9`, **verified on hardware 2026-07-30** (5th attempt) ·
**Severity:** medium (reads as a crash to anyone watching) · **Verified** 2026-07-29

`handleSerialCommands()` has two dispatch paths, and only one of them hands the
display back. The queued `/cm` path parses and returns:

```cpp
if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) {
    bool result = serialCli.parse(String(packet.text));
    xQueueSend(rspQueue, &result, 0);
    ...                                   // returns — no backToMenu()
}
```

while the `serialDevice` (BLE) path ends with:

```cpp
if (!cmd_trimmed.startsWith("nav") && !cmd_trimmed.startsWith("option")) { backToMenu(); }
```

So a verb sent over BLE repaints the menu afterwards and the same verb sent over HTTP
does not — the screen keeps whatever frame the verb last drew
(`serialcmds.cpp:41-54` versus `:71-77`).

**Observed.** `evilportal` was dispatched over `POST /cm`, ran, and was exited on the
device. The radio side confirmed it had fully stopped — no APs on air — while the
display sat on **"Shutting down…"** indefinitely. The firmware was healthy throughout;
only the frame was stale. Any button press repaints it.

**This is a gap in `7c1c2ce7`, not a new defect.** That commit fixed exactly this
symptom — its message even names *"Evil Portal appearing stuck on Shutting down…"* —
but only for the path that reads from `serialDevice`. The HTTP queue path was missed.

**Why it is worth fixing rather than filing as cosmetic:** it is indistinguishable
from a crash to anyone looking at the device. During this session the operator sitting
in front of the board, with full context, reached for the reset button. An app
operator watching from across a room will read it as a hang every time — and any app
driving this board over HTTP hits it on **every** command that draws.

**Fix:** hoist the `backToMenu()` call so both dispatch paths share it, keeping the
existing `nav`/`option` exemption. Roughly five lines.

**Fix applied in `d71f19e9`.** `redrawUnlessNavigation()` (`serialcmds.cpp:41-48`) is
now called from both dispatch paths — the HTTP queue path at `:71` and the
`serialDevice` path at `:90` — keeping the `nav`/`option` exemption. It is
verb-agnostic.

**⚠️ NOT YET VERIFIED ON HARDWARE.** Three attempts were made 2026-07-30 and none
produced a valid observation. Recorded in full because two of the three failures are
themselves findings:

| Attempt | Outcome |
|---|---|
| 1 | **Invalid.** The operator pressed RST at 00:34:19; the dispatch went out at 00:34:43, 24 s after the AP had already gone. Console shows `rst:0x1 (POWERON)` and **no** crash markers. |
| 2 | **Blocked.** `POST /login` could not complete — the ISSUE-12 failing case, see the variance table in that entry. |
| 3 | **Invalid, and badly designed.** The device was parked in the **Config submenu**, which is precisely where the repaint cannot work — see the limitation below. The main loop then wedged (ISSUE-30). |

**The fix is narrower than it looks — `backToMenu()` only takes effect on the main
menu.** `backToMenu()` is a single flag set, `returnToMenu = true` (`utils.cpp:26`);
it draws nothing. The flag is consumed, and the repaint performed, inside
`loopOptions()` — but only under `if (menuType == MENU_TYPE_MAIN)`
(`display.cpp:568`, consumption at `:584-588`). Every other menu level checks
`returnToMenu` only *after* its own `loopOptions()` returns, which requires a button
press (e.g. `ConfigMenu.cpp:18-24`).

So a `/cm`-dispatched verb repaints the screen **only if the main loop happened to be
sitting on the main menu**. If the operator has navigated into any submenu, the
request is set and silently dropped until they press something — which is the same
class of symptom the fix was written to remove. This is not a defect in `d71f19e9`,
which does what it says; it is a limit on how much that mechanism can achieve, and it
was not understood when the fix was written.

**What a valid test requires**, established the hard way:

- The device must be on the **main menu**, not a submenu.
- The verb must actually **draw**, or there is nothing to repaint over.
- The verb must not set `returnToMenu` itself, or the test passes regardless of the
  fix and proves nothing. `evilportal` qualifies — `evil_portal.cpp:95` only *reads*
  the flag. `find_i2c_addresses()` does **not** qualify (`i2c_finder.cpp:33` sets it),
  and is unreachable from the CLI anyway: the `i2c` verb is wired to a different,
  text-only callback (`util_commands.cpp:448`).

That leaves `POST /cm cmnd=evilportal` as the only discriminating test available, which
is also the original repro.

---

**VERIFIED 2026-07-30 on ELF `76d42c72f2b4a8a4`. The fix works.**

**The paragraph above is wrong, and being wrong is what cost four attempts.**
`evilportal` is not "the only discriminating test" — it is **structurally incapable**
of being one. Exiting the blocking portal *requires* button presses (Esc chord →
"Exit Portal"), and this entry itself states *"Any button press repaints it."* So the
observation can never be attributed to the fix rather than to the operator's own
input. Attempts 1–4 were all chasing an unfalsifiable test.

**Attempt 4 (2026-07-30) failed on exactly that, plus something worse: the portal
never actually exited.** The operator reported the screen returning to the main menu,
which looked like success. It was not:

- the portal AP was **still on air** afterwards, and the portal **still served**
  `GET /hotspot-detect.html` → 200, 99 B, 0.011 s;
- `EvilPortal::shutdown()` calls `webServer.end()` and `dnsServer->stop()`, so a
  serving web server **proves** `shutdown()` never ran and `loop()` never returned;
- BLE `uptime` returned 0 bytes with no EOT, twice at 25 s, while GATT connect and
  service discovery still succeeded — the serial task was still inside the portal;
- zero crash/reset markers, so no reboot either.

`redrawUnlessNavigation()` therefore never ran. What the operator saw was the **main
loop task repainting over a still-running portal** — worth knowing on its own: *this
device can display the main menu while a portal is live, serving, and holding the
serial task.* That observation is what upgraded ISSUE-31 to hardware-verified.

**Attempt 5 changed the probe verb to `blespam apple 10` and settled it immediately.**
A valid ISSUE-24 probe needs three properties, all confirmed before dispatch:

1. **it draws** — `drawMainBorderWithTitle`, `padprintln`, `tft.fillRect`
   (`ble_spam.cpp:379-382`, `:799-802`);
2. **it returns on its own** — finite packet count, no on-device interaction;
3. **it does not set `returnToMenu` itself**.

Run, 09:52–09:55: fresh boot (uptime 00:00:52), `webui -bg` at t=55,132 ms reaching
`post-begin` at dma largest 6,644; static IP; `POST /login` 302 + cookie in 2.35 s;
operator confirmed the device was on the **main menu**; `POST /cm cmnd=blespam apple 10`
→ `queued`, 200 in 28 ms. **The operator was instructed not to touch the device and
confirmed they did not.** The spam drew, completed, and the BLE API resumed by itself
— which proves `blespamCmdCallback` returned and so `redrawUnlessNavigation()` ran.

Result: **the screen repainted to the main menu unaided.** Controls: BLE `uptime`
answered `00:01:56` against the 00:00:52 baseline, i.e. **monotonic, no reboot**, and
the console capture contained zero `rst:0x` / `Backtrace:` / `assert failed` / `Guru`
markers. With no button press and no reboot, the only remaining mechanism is
`redrawUnlessNavigation()` → `backToMenu()` → `returnToMenu` consumed by
`loopOptions()` under `MENU_TYPE_MAIN`.

**The submenu limitation documented above is unchanged and still applies.** A verb
dispatched over `/cm` repaints only when the main loop is sitting on the main menu.

**Reusable test-design rule this produced:** *to verify a repaint, never pick a probe
verb that needs human input to terminate — the input becomes an alternative
explanation.* Prefer a self-terminating drawing verb such as `blespam <type> <count>`.

---

### ISSUE-25 — the BT controller aborts the device when internal DMA runs out

**Status:** OPEN · **Severity:** critical (takes the device down) · **Verified**
2026-07-30 · **ELF-matched decode; non-fatal form reproduced separately**

Under sustained memory pressure the Bluetooth controller fails an internal allocation,
and emitting the log line about that failure requires **another** allocation which also
fails, reaching `abort()`.

**Decoded backtrace**, innermost last. `ELF file SHA256: 76d42c72f` matches the flashed
binary, so this decode is authoritative:

```
abort() was called at PC 0x40378e97 on core 0

malloc_internal_wrapper        (bt.c.obj)      <- BT controller allocation FAILED
  malloc_retention_wrapper
    esp_log / esp_log_va                       <- it tried to log the failure
      vprintf -> __sfvwrite_r -> __swrite
        console_write -> uart_write
          _lock_acquire_recursive
            lock_init_generic                  <- abort()
```

Observed at **+342 s** into a portal run with the BLE API armed. The device rebooted
cleanly and came back healthy: free 80,811, dma 31,732, BLE re-armed, portal stopped.

**Same final mechanism as ISSUE-18, different trigger.** ISSUE-18 reaches
`lock_init_generic` → `abort()` through `fopen` from `BruceConfig::saveFile`; this
reaches it through the BT controller's allocator. The shared property is that **newlib
aborts rather than returning an error when it cannot allocate a lock**, so any
allocation failure on a path that then logs can take the device down.

**The non-fatal form of the same condition, captured 2026-07-30.** In the ISSUE-12
run-2 failing state the console recorded:

```
E (309086) BLE_INIT: Malloc failed
E (309149) BLE_INIT: Malloc failed
```

and the device **survived** — because here the log call itself succeeded. So the abort
is the tail of a spectrum, not a distinct fault: the same BT-controller allocation
failure either logs and continues, or recurses into `abort()`, depending on whether
there is enough memory left to emit the message. `BLE_INIT: Malloc failed` on the
console should therefore be read as "the device is one allocation from rebooting".

**Precondition, measured:** free heap in the 12–17 KB band with the largest DMA block
under ~8 KB. Both observations sit there.

**Fix direction (not implemented):** the device cannot prevent newlib's abort, so the
only real defence is not entering the band — refuse to arm a second radio consumer
below a DMA-block floor, in the spirit of `radioHasMemForBle()`'s existing 15 KB gate
(`radio_mem.h:32`).

---

### ISSUE-26 — the BLE GATT *write* is rejected under memory pressure, not just the reply

**Status:** OPEN · **Severity:** high (loses the command, not just the answer) ·
**Verified** 2026-07-30

ISSUE-16 documents replies truncating or arriving empty while the command still
executes. This is strictly worse: the **write itself** is refused at the GATT layer, so
the command never reaches the device at all.

Observed as a precursor to the ISSUE-25 abort, from `bleak`:

```
BleakGATTProtocolError: (BleakGATTProtocolErrorCode.UNLIKELY_ERROR: 14,
                         'GATT Protocol Error: Unlikely Error')
```

`0x0E` (Unlikely Error) is the ATT error the NimBLE stack returns when it cannot
service the write.

**Why this matters to the app, and why it is the opposite of ISSUE-16's guidance.**
ISSUE-16 says an empty reply means the side effect *has* happened, so do not retry.
Here the side effect has **not** happened, and a retry is correct. The two are
distinguishable at the client: ISSUE-16 is a successful write with a short or empty
notify; ISSUE-26 is a write that raises. An app must branch on that, or it will either
double-execute commands or silently drop them.

**Not established:** the exact heap threshold, and whether the rejection is ever
transient enough for a retry to succeed without first freeing memory.

---

### ISSUE-28 — `beginAP()` starts the DNS and HTTP servers even when the AP failed to come up

**Status:** OPEN (blocking verb only) · **Severity:** medium · **Verified** by code
2026-07-30

`EvilPortal::beginAP()` captures whether the AP actually started and then ignores it
(`evil_portal.cpp:137-152`):

```cpp
_apOnAir = WiFi.softAP(apName, emptyString, _channel);
if (!_apOnAir) {
    Serial.printf("[PORTAL] softAP failed for SSID '%s' on ch%d\n", ...);   // reaches nobody
}
wifiConnected = true;                 // set regardless
...
setupRoutes();
dnsServer->start(53, "*", WiFi.softAPIP());
webServer.begin();                    // all three unconditional
```

Three separate problems compound:

1. **The failure is reported only through `Serial`**, which per ISSUE-22 reaches nothing
   on this board — not the console, not the app.
2. **`wifiConnected = true` is set unconditionally** (`:144`), so the rest of the
   firmware believes an AP exists.
3. **A failed start still claims port 53 and starts the web server** (`:149-151`),
   leaving the shared DNS server bound with no AP behind it. `karma` shares that DNS
   server, so the leak is cross-feature.

The same unconditional sequence appears a second time on the restart path
(`:283-288`), which does not even capture `softAP()`'s return.

**Scope — the background path is already guarded, the blocking verb is not.** The
headless work added `_apOnAir` and the `apOnAir()` accessor precisely so
`evilportal -bg` could report a failed start instead of a false success (commit
`4c4378a1`). That guard is in the *caller*. `beginAP()` itself is unchanged, so the
**blocking `evilportal` verb still commits all of this state before knowing whether the
AP exists**.

**Fix direction:** return `bool` from `beginAP()`, skip `:149-151` and the
`wifiConnected` assignment when `softAP()` failed, and use `log_e` rather than
`Serial.printf`.

**Fix applied 2026-07-30, ELF `411d7e151dbc2356`** — exactly that direction, plus the
restart path the entry flagged as a second site.

- `beginAP()` returns `bool`. On a failed `softAP()` it now returns early **before**
  `wifiConnected = true`, `setupRoutes()`, `dnsServer->start(53, ...)` and
  `webServer.begin()`, so a failed start no longer claims port 53 on the DNS
  singleton that `karma` also borrows.
- Both diagnostics moved `Serial.printf`/`Serial.println` → `log_e`, so they reach the
  console instead of nothing (ISSUE-22). `CORE_DEBUG_LEVEL=1` on this board compiles
  out every level below ERROR, so `log_e` is the only option, not a stylistic choice.
- `restartWiFi()` now **captures** `softAP()`'s return, which it previously discarded
  entirely, and bails out the same way rather than rebuilding DNS and HTTP on top of
  an AP that is not there.
- The constructor no longer enters `loop()` when the AP failed — that would sit the
  operator in front of a portal UI for an attack that can never receive a client,
  while holding the serial task.

**The `restartWiFi()` half is now HARDWARE-VERIFIED (2026-07-30, ELF
`411d7e151dbc2356`). The `beginAP()` half is still code-only.**

**How the failure was induced** — no memory pressure needed, and it is deterministic.
The portal's own `/ssid` route assigns `apName` straight from the query argument with no
validation (`evil_portal.cpp:267-270`), and the background tick honours the resulting
`_pendingWifiRestart` (`:430-433`). So from a client on the portal AP:

```sh
curl "http://172.0.0.1/ssid?ssid="      # empty SSID
```

drives `WiFi.softAP("")`, which fails unconditionally at Arduino core `AP.cpp:219`
(`if (!ssid || *ssid == 0)`). Console captured live:

```
[E][AP.cpp:220] create(): SSID missing!
[E][evil_portal.cpp:313] restartWiFi(): [PORTAL] softAP failed on restart for SSID '' on ch6
```

That capture proves four things at once: `softAP()` genuinely returned false; **the fix's
new code ran** — `restartWiFi()` now captures a return value it previously discarded
outright, so this log line cannot exist pre-fix; **`log_e` reaches this board's console**,
which is the diagnostic half of the fix (ISSUE-22 predicted it, this measures it); and the
DNS/HTTP rebuild was skipped, since the `return` at `:315` is unconditional and
immediately follows the log that fired, with no branch in between. External confirmation:
AP absent from an `nmcli` scan (0 entries), `GET /hotspot-detect.html` → `http=000`
curl exit 28, laptop deassociated, uptime monotonic 00:37:52 → 00:49:36, zero crash
markers in the whole capture.

⚠️ **`beginAP()`'s own failure branch remains UNVERIFIED**, and it is a different call
site. It **cannot be induced through the CLI**, which is why: `attack_commands.cpp:53`
substitutes `"Free Wifi"` for an empty SSID, and `setup()`'s autoMode branch does it
again (`evil_portal.cpp:86`), so the empty-SSID trick cannot reach it. **Measured, so the
next person does not repeat it: a 40-char SSID does NOT make `softAP()` fail** — the
portal started normally (`free_heap:17079 dma_block:8692`). Arduino truncates to 32 bytes
and ESP-IDF accepts the oversized `ssid_len`. Reaching `beginAP()`'s branch needs genuine
resource exhaustion, or a temporary test hook. The constructor's `if (!apUp) return;`
guard is likewise unexercised.

---

### ISSUE-29 — ~~`POST /cm cmnd=nav` latches button globals that the main menu never clears~~ (premise refuted)

**Status:** **WITHDRAWN 2026-07-30 — the defect does not exist** · **Severity:** was medium
· **Premise refuted by code**, see §Why the premise is false below

Kept rather than deleted because the refutation is the useful part, and because a
harmless fix was shipped against it before the premise was checked
(`webInterface.cpp:564-566` now clears the flags explicitly). Neither OPEN nor RESOLVED
fits: there was nothing to fix, so nothing was proven by fixing it. **No open work
remains here** — do not plan a re-test.

The `nav` handler sets the button globals and never resets them
(`webInterface.cpp:537-552`):

```cpp
auto tmp = millis() + time;
while (tmp > millis()) {
    AnyKeyPress = true;
    SerialCmdPress = true;
    *var = true;                                   // never set back to false
    if (!LongPress) vTaskDelay(pdMS_TO_TICKS(190));
    else vTaskDelay(pdMS_TO_TICKS(50));
}
```

It relies entirely on a consumer calling `check()`, which clears the flag as it reads
it. **On the main menu that consumer does not run for `EscPress`** — the check is
gated (`display.cpp:647`):

```cpp
if (menuType != MENU_TYPE_MAIN && check(EscPress)) { index = -1; break; }
```

So `POST /cm cmnd=nav esc` delivered while the device sits on the main menu latches
`EscPress` **true indefinitely**. `AnyKeyPress` and `SerialCmdPress` are set on every
`nav` regardless of target and have no consumer on this path either.

Note also the default hold is 10 ms (`else time = 10`) while the loop body delays
190 ms, so exactly one iteration always runs — the "hold duration" is not what it
appears to be for anything except the `…0`-suffixed forms.

**Consequence:** a latched `EscPress` is consumed by the *next* menu the operator opens,
which will appear to exit itself immediately for no visible reason. This is a plausible
contributor to ISSUE-30 but has **not** been shown to cause it.

**Confirmed on the wire while investigating**: `nav` really is special-cased in the
AsyncWebServer task *before* queueing — the reply is `command nav esc success`, not
`command <verb> queued`. That upgrades a previously code-only claim in `BRUCELINK.md`.

**Fix direction:** clear the flags after the hold loop, or give the main-menu path a
consumer for `EscPress`.

**Fix applied 2026-07-30, ELF `411d7e151dbc2356`** — the first option. `*var`,
`AnyKeyPress` and `SerialCmdPress` are all set back to `false` immediately after the
hold loop, so a `nav` pulse is now a bounded press-then-release instead of a latch.

**This is predicted to also improve ISSUE-19, and that prediction is worth testing.**
ISSUE-19 established that a single `nav` pulse cannot release a blocked verb, and
attributed it to `ScrollableTextArea` needing two edges:

```cpp
while (check(SelPress))  { ... }   // wait for RELEASE
while (!check(SelPress)) { ... }   // then wait for PRESS
```

With the flag latched high forever, the **release-wait could never complete**. Giving
the pulse a falling edge supplies exactly what that first loop is waiting for. So the
latch may well have been the *cause* of "one pulse is not enough", not merely a
co-symptom.

⚠️ **TESTED 2026-07-30 — the prediction FAILED, and this entry's whole premise is
FALSE. `nav` never latched anything.** Kept rather than deleted, because the wrong model
survived two sessions and the refutation is the useful part.

**The measurement.** Fresh boot, `webui -bg` with no navigation (`webui post-begin` dma
largest **7,156**, the best profile recorded to date), both BLE characteristics
subscribed. Baseline `uptime` answered in 60 ms. `ap_info` dispatched → `COMMAND: ap_info`
and `{"type":"state","device_state":"ap_info"}` events, then no EOT for 12 s; a second
`uptime` queued behind it stayed unanswered for 10 s, confirming the serial task was
blocked. Then **exactly one** `POST /cm cmnd=nav sel` (`command nav sel success`, http
200) — and it was **still blocked 25 s later**. See ISSUE-19 for what the pulse count
actually is.

**Why the premise is false: two independent mechanisms already cleared those flags.**

1. **A dedicated task wipes them every ≤75 ms.** `taskInputHandler` (`src/main.cpp:84-111`,
   created unconditionally at `:533-540`, priority 2, 10 ms tick, a weak symbol no board
   overrides) sets `SelPress`, `EscPress`, `AnyKeyPress`, `SerialCmdPress` and every other
   button global to **false** on each pass, gated only by
   `if (!AnyKeyPress || millis() - timer > 75)`. It does this whether or not any consumer
   calls `check()`.
2. **`check()` clears on read.** It lives at `include/globals.h:244-283` — an
   `extern inline` taking `volatile bool&`, which is why grepping `src/` for
   `bool check(` and searching the ELF symbol table both come up empty. Body:
   `if (!btn) return false; vTaskSuspend(xHandle); btn = false; AnyKeyPress = false;
   SerialCmdPress = false; delay(10); vTaskResume(xHandle); return true;`

So `EscPress` cannot be latched "true indefinitely" — it is false again within 75 ms. The
fix is **harmless but addresses a defect that does not exist**, and that is exactly why
its predicted effect on ISSUE-19 did not appear.

**Two further claims in this entry fall with the premise:**
- The "Consequence" paragraph — a latched `EscPress` making the *next* menu the operator
  opens exit itself — cannot happen. No operator opens a menu within 75 ms.
- The "plausible contributor to ISSUE-30" link is therefore **withdrawn**.

**What is still true** is the mechanical observation the entry opens with: the handler
does not reset `*var` itself, and the default hold really is 10 ms against a 190 ms loop
body, so exactly one iteration runs for anything but the `…0`-suffixed forms
(`webInterface.cpp:546-548`). Also confirmed on the wire and worth keeping: `nav` is
special-cased before queueing, replying `command nav esc success` rather than
`command <verb> queued`.

---

### ISSUE-30 — the main loop task can wedge while every other task keeps running

**Status:** OPEN · **Severity:** high (device is unusable at the board; remote surfaces
still answer) · **Observed once** 2026-07-30 · **ROOT CAUSE NOT ESTABLISHED**

The device became unresponsive at the board — static screen, dead buttons — while
remaining fully responsive over both remote transports. Only a physical RST recovered
it.

**Evidence, all captured before the reset:**

| Probe | Result |
|---|---|
| Screen | Main menu displayed |
| Status-bar clock | **frozen** across >30 s |
| Physical buttons | no effect, operator confirmed |
| BLE `uptime` | **answers in 60–122 ms**, value advancing (00:05:42 → 00:13:32) |
| `POST /cm` | `command uptime queued`, kept working |
| Console | **no** abort, Backtrace, assert, Guru, watchdog or unplanned `rst:0x` |

The frozen clock is the load-bearing observation: `drawStatusBar()` runs
unconditionally every 30 s inside `loopOptions()` on `MENU_TYPE_MAIN`
(`display.cpp:592-595`), independent of input. A frozen clock therefore means the main
loop task is **blocked**, not merely ignoring input. BLE parsing runs on the serial
task, which is why the CLI kept answering.

**Memory was tested as a hypothesis and REJECTED.** At freeze time heap was collapsing
(free 15,015 → 11,267; dma largest 6,900 → 2,804; `minEver` 1,211), so "cannot allocate
to draw" was plausible. Dropping the station and running `webui -off` restored free to
**58,015** and dma largest to **19,444** — and the screen stayed frozen and the buttons
stayed dead. Recorded because it is a clean falsification, not a guess.

**Excluded:** `checkReboot()`, the first call on the `MENU_TYPE_MAIN` path, is an empty
stub on this board (`mykeyboard.cpp:1454`).

**Candidates, none discriminated:** a blocking `options[chosen].operation()`
(`display.cpp:738-748`); a spin inside `ConfigMenu::optionsMenu()`'s `while (true)`; a
latched `forceMenuOption`; a latched button global (ISSUE-29); or a leaked `tftMutex`
reference (below).

**Candidate added 2026-07-30: a leaked `tftMutex` reference.** Every take uses
`portMAX_DELAY` (`lib/TFT_eSPI/TFT_eSPI.cpp:96, 99, 110, 113, 165, 170`), so a reference
that is taken and never given blocks **every** other task at `begin_tft_write` forever.
That would produce this entry's whole evidence table — static screen, frozen clock, dead
buttons, BLE answering because it runs on the serial task, no panic and no watchdog — and
it explains the one result that defeated the memory hypothesis: **a mutex wait is not a
memory condition**, so freeing 43 KB could not have helped. The timing also fits: the
mutex was re-scoped in `2d9422ea` on 2026-07-29 19:41 and this wedge was seen 2026-07-30.

**Marked SUSPECTED and ranked below the others, because the audit that raised it also
failed to find a leak path.** The begin/end imbalance in that file runs in the *safe*
direction — 57 gives against 32 takes, so it over-releases rather than leaking — and no
take-without-give site was identified. Treat this as "the mechanism that would explain
the symptoms if a leak exists", not as evidence that one does.

**Cheap discriminator if it recurs:** a wedged main loop with BLE alive is already the
signature; add `free` — if it answers, the serial task holds no display reference, and a
`tftMutex` leak is then the *only* candidate consistent with the main loop being stuck at
`begin_tft_write`.

**Why no backtrace exists:** nothing panics and no watchdog fires, so there is no
automatic dump. The one line that would discriminate the first candidate,
`Serial.println("Selected: " + …)` at `display.cpp:744`, goes to `Serial`, which per
ISSUE-22 reaches nothing on this board. **Changing that one line to `log_e` is the
cheapest next experiment** and would likely settle it.

**Diagnostic added 2026-07-30, ELF `411d7e151dbc2356` — the experiment is now armed,
but the wedge has not recurred, so nothing is settled yet.** `display.cpp` emits two
`log_e` lines around the menu dispatch instead of the unreachable `Serial.println`:

```
Selected: <label>          <- before options[chosen].operation()
Returned from: <label>     <- after it
```

The pair is deliberate. A `Selected:` with no matching `Returned from:` names the
menu entry that never came back and discriminates the "blocking
`options[chosen].operation()`" candidate directly; both lines present move suspicion
to `ConfigMenu::optionsMenu()`'s `while (true)` or a latched flag. The "Forcely "
prefix for `forceMenuOption` dispatches is preserved.

`log_e` rather than `log_i` because `CORE_DEBUG_LEVEL=1` on this board
(`boards/smoochiee-board/smoochiee-board.ini:21`) compiles out every level below
ERROR — the level is a constraint here, not a claim about severity.

**Next time the main loop wedges, capture `/dev/ttyACM0` and read the last
`Selected:` line.** One candidate in the list above is also now less likely: ISSUE-29's
latched `EscPress` was fixed in the same build, so if the wedge persists, that was not
the cause.

**Trigger sequence, for repro attempts.** Order matters, and note **no `nav` was sent
before the freeze** — the `nav esc` pulses came afterwards and did unwind the UI back
to the main menu without unwedging it:

1. Fresh boot; `webui -bg` at `t=20 s`.
2. Laptop associated by static IP; `POST /login` OK.
3. `POST /cm cmnd=uptime` → queued. This fired `backToMenu()` while the device was on
   the **main menu**.
4. Operator navigated into the **Config submenu**.
5. Freeze.

**Not the same as ISSUE-11.** Those were unexplained *reboots* with no console capture.
This is a *hang* with a full capture that shows no reset at all.

**Calibration worth keeping:** on this board the RST button produces
`rst:0x1 (POWERON)`, while a software `reboot` produces `rst:0xc (RTC_SW_CPU_RST)`. The
two are distinguishable in any capture, which is how an operator reset was told apart
from a firmware fault during this session.

---

### ISSUE-31 — Evil Portal's destructor is empty, so the portal leaks its whole AP + DNS + HTTP stack

**Status:** FIXED in code 2026-07-30, fix UNVERIFIED · **Severity:** raised low → **high** ·
**Defect HARDWARE-VERIFIED 2026-07-30** (was code-only)

`EvilPortal::~EvilPortal()` is empty (`evil_portal.cpp:38`). `karma`'s
`destroyActivePortal()` (`karma_attack.cpp:684`, called from six sites) deletes the
portal object, so nothing releases the DNS server bound on port 53 or restores the WiFi
mode. Same class as ISSUE-28's failure path, but in upstream code rather than this
fork's.

**`shouldTerminate()` is dead code.** `evil_portal.cpp:419` defines it and
`evil_portal.h:57` declares it; a tree-wide grep finds **no callers**. It is worth
naming explicitly because it looks like it would explain the portal exiting unprompted
that this register has recorded as unexplained — **it does not**, because it never
runs. That observation stays open.

~~**Left unfixed on purpose:** both sit in upstream code outside the headless-portal
change, and this fork prefers additive changes in new files to keep merges clean.~~

---

**Severity raised and the defect confirmed on hardware 2026-07-30 — it is much worse
than "karma leaks the DNS server".**

Caught while attempting the ISSUE-24 verification. A **blocking `evilportal`** was
left without completing "Exit Portal", and afterwards:

| Probe | Result |
|---|---|
| AP `Free Wifi` on air | **yes**, ch 6, seen by an independent `nmcli` scan |
| `GET /hotspot-detect.html` | **200, 99 bytes, 0.011 s** |
| `GET /generate_204` | **302** |

So the AP, the port-53 DNS server and the web server all keep running and **keep
serving**, indefinitely, after the operator believes the attack is over. This is not
a tidy-up nit: **a captive portal that outlives its own UI is still capturing.** The
screen had meanwhile been repainted to the main menu by the main loop task, so
nothing on the device indicated a portal was live.

It is also the mechanism behind the BLE symptom in that session — the leaked stack
holds heap, so replies could not be allocated (ISSUE-16).

**Fix applied, ELF `411d7e151dbc2356`.** `~EvilPortal()` now calls `shutdown()`, and
`shutdown()` was made **idempotent** (`_shutdownDone`) so the paths that already call
it explicitly — `evilPortalBgStop()`, and the "Exit Portal" path — are unaffected. A
`_beganAp` flag prevents a constructor that bailed out of `setup()` from tearing down
radio state it never touched.

⚠️ **The verification design recorded here was IMPOSSIBLE — corrected 2026-07-30.**
It said: start the blocking `evilportal`, leave it without completing "Exit Portal",
re-probe the AP and `GET /hotspot-detect.html`, and both must fail. **That test can never
pass, and not because the fix is broken.**

`attack_commands.cpp:95` constructs a **temporary** `EvilPortal(...)`. A temporary is
destroyed when the full expression ends — i.e. when the constructor returns — and the
constructor only returns when `loop()` returns (`evil_portal.cpp:40`). Verified by
inspection: `EvilPortal::loop()` (`:329-399`) contains exactly **two** `return`s — the
`_backgroundMode` guard at the top and the one inside the "Exit Portal" branch
(`:385-390`) — and **no `break`**. So "left without completing Exit Portal" means `loop()`
never returns, the temporary is never destroyed, and the destructor **cannot** run. The
old observation of a portal still serving after such an exit is explained by `loop()`
still running, not by the empty destructor.

**What the destructor actually covers for the blocking verb** is the `if (!apUp) return;`
early return at `:39` — a portal whose AP failed now tears down on the way out instead of
leaking. That path is unreachable through the CLI for the same reason given in ISSUE-28.

**VERIFIED instead: `shutdown()` idempotency, which was the change's real regression
risk.** `evilportal -off` → `stopPortal()` calls `shutdown()` explicitly, then `delete`
runs the destructor which calls it again. Result on hardware: portal stopped cleanly, heap
returned to its normal post-portal plateau (`free_heap:53143 dma_block:18420`), no crash
markers, uptime monotonic. **Quantitative evidence the `_shutdownDone` guard actually
fired: the stop took 692 ms**, which is one teardown's worth of the 500 ms of `vTaskDelay`
in `shutdown()` — a second full pass would have shown ~1,190 ms.

⚠️ **Still unverified:** the `karma` path (`destroyActivePortal()`), which is the case this
entry was originally opened for.

**The real remaining defect this exposed is not the destructor at all: the blocking
`evilportal` has exactly one exit, and it is a menu selection.** No timeout, no remote
stop, no `returnToMenu` check. That belongs to ISSUE-6/ISSUE-1's headless-entry-point work.

**Update 2026-07-30 (`cedad77f`):** `shouldTerminate()` — the function this entry pointed
at as the one that *would* have given `loop()` a time-based exit — has been **deleted**,
not wired up. See ISSUE-35 for why: with no caller for `setBaseDuration()` on the blocking
path it would have self-exited the portal after 15 seconds. So the blocking `evilportal`
still has exactly one exit, a menu selection, and the remedy is now explicitly the headless
entry point rather than a revived duration policy. The unexplained self-exiting portal
remains unexplained; nothing above touches it.

---

### ISSUE-36 — the portal's DHCP server hands out no default gateway

**Status:** OPEN · **Severity:** low (captive-portal detection is unaffected; anything
needing a route is not) · **Verified** on hardware 2026-07-30

`beginAP()` configures the AP with `WiFi.softAPConfig(apGateway, apGateway,
IPAddress(255,255,255,0))` (`evil_portal.cpp:154`), passing the same address as both local
IP and gateway. The DHCP server that results **omits the routers option entirely**. Raw
options from a client that explicitly asked for it (`nmcli -f DHCP4 con show`):

```
requested_routers = 1                      <- client asked
ip_address = 172.0.0.2
dhcp_server_identifier = 172.0.0.1
domain_name_servers = 172.0.0.1
dhcp_lease_time = 7200 · interface_mtu = 1500 · broadcast_address = 172.0.0.255
```

No `routers = …` in the reply, and `nmcli` shows `IP4.GATEWAY: --`. Clients get an address
and a DNS server but **no default route**.

Captive-portal detection survives this because the hijacked DNS points the probe at
`172.0.0.1`, which is on-link (see ISSUE-27) — so this is not what breaks the handset
experience. It does mean any client behaviour that depends on having a default route will
not work, and it is the fact the old ISSUE-27 text misread as "a lease with gateway
172.0.0.1".

---

### ISSUE-37 — the portal serves only one handset at a time

**Status:** OPEN · **Severity:** medium · **Observed** 2026-07-30 · **Mechanism
SUSPECTED, not verified**

With an iPhone associated to the portal AP, an Android phone **could not join at all**.
After the network was forgotten on the iPhone, the Android associated normally. Reported
by the operator during the ISSUE-27 handset test.

`WiFi.softAP(apName, emptyString, _channel)` leaves `max_connection` at the Arduino
default of 4 (`AP.cpp:236`), so the AP is not configured to refuse a second station.

**Suspected cause is memory, not configuration:** the portal starts at roughly
`free_heap:17000 dma_block:6100` with the BLE API armed, and a second station's driver
buffers plausibly cannot be allocated from that. Consistent with the console showing
`E BLE_INIT: Malloc failed` twice during the same session and `minEver` reaching **32
bytes**. **Not verified** — no allocation-failure log names the station path, and the
association failure was observed once, from the client side only.

**Next experiment:** repeat with `ble api off` for the extra ~62 KB and see whether two
stations associate. If they do, this is a memory-capacity limit and belongs with ISSUE-21
rather than being its own defect.

---

### ISSUE-39 — `reverseshell` leaves its AP on air after a clean exit

**Status:** **FIX SHIPPED, AWAITING OPERATOR VERIFICATION** (ELF `46d975be7d38f128`) ·
**Severity:** medium (an attack AP keeps broadcasting after the operator ended the attack,
and holds ~63 KB) · **Verified** on hardware 2026-07-30

> **Fix, 2026-07-30:** `wifiDisconnect()` on **all four** exits of `ReverseShell()`, not
> just the clean one — `WiFi.mode(WIFI_AP)` is set at `reverseShell.cpp:82` *before* the
> `softAPConfig` and `softAP` failure returns, so those two also returned with the radio
> armed. `wifiDisconnect()` (`wifi_common.cpp:152-164`) does `softAPdisconnect(true)` +
> `disconnect(true, true)` + `WIFI_OFF`, which is the outcome this entry asked for.
>
> ⚠️ **NOT verified on hardware — two attempts were made and both were defeated by
> ISSUE-41, not by this fix.** The verb blocks the serial task *and* binds port 80, so
> neither BLE nor `/cm nav esc` can reach it; the only exit is the on-device
> **LEFT+RIGHT** chord. Both operator attempts ended in a reboot, and the captured
> backtrace put the fault on the **main loop task** inside `loopOptions()` — the presses
> were driving the main menu, not `reverseshell`'s Esc check. This is
> **a code-verified fix only.**
>
> **Two things were learned that the next attempt needs.** The chord is *racy* while a
> verb blocks the serial task, because the main loop is still consuming button presses
> (ISSUE-41); and on a dimmed screen the first press is swallowed by `wakeUpScreen()`,
> so it takes two. ISSUE-41 is now fixed, which removes the reboot but **not** the race.
>
> **The ordering in this fix was itself corrected once.** The first version called
> `wifiDisconnect()` with no delays, immediately after `ws->closeAll()` — which only
> *queues* close frames for the AsyncTCP task — and left `~AsyncWebServer()` to delete the
> WebSocket after `WIFI_OFF`. It now spaces the steps and calls `webServer.reset()` while
> lwIP is still up, matching `EvilPortal::shutdown()` (`evil_portal.cpp:400-410`), the only
> teardown proven on this board. **No evidence was ever produced that the first version
> crashed** — it was assumed to be the cause of the first reboot before the backtrace
> existed, and the backtrace later pointed at ISSUE-41 instead.

Exposed the moment ISSUE-38 stopped crashing: nothing had ever survived the exit long
enough to see what it left behind.

The exit path (`reverseShell.cpp:213-220`) stops the TCP server, closes the websockets,
ends the web server and stops DNS — but **never brings the radio down**. There is no
`WiFi.softAPdisconnect()`, no `WiFi.mode(WIFI_OFF)`, no `wifiDisconnect()`. So the verb
returns to the CLI with its AP still up.

**Measured after a verified-clean exit** (ELF `f5244eb35dd10795`, device responsive,
uptime continuous at `00:21:06`):

| Check | Result |
|---|---|
| `nmcli` scan A | `BruceShell` ch 1, signal **94** |
| `nmcli` scan B, 5 s later | `BruceShell` ch 1, signal **92** |
| Laptop association *after the verb exited* | **succeeded** |
| Free heap | **18,387** (vs **81,327** at boot) — largest 7,156, dma 6,644 |

Two independent scans and a successful association rule out a stale scan cache. The
~63 KB gap is the WiFi AP stack still resident.

This is the same class as ISSUE-31 (Evil Portal's empty destructor leaking its whole AP +
DNS + HTTP stack), one module along, and it has the same consequence: the device looks
idle to the operator while still broadcasting a WPA2 AP whose passphrase is compiled in
(`REVERSE_SHELL_AP_PASSWORD`, `reverseShell.h:10`).

**Fix direction:** tear the radio down on the way out, matching what `EvilPortal::shutdown()`
does — and note that `wifiDisconnect()` forces `WIFI_OFF` (`wifi_common.cpp:159`), which is
the right outcome here. **Not fixed in `cedad77f`**: it is a new finding rather than part
of that batch's scope, and it wants its own verified flash cycle.

---

### ISSUE-40 — `restartWiFi()` duplicates the portal's whole route table on every rename

**Status:** **RESOLVED 2026-07-30** — `webServer.reset()` added after `end()` in
`restartWiFi()`; measured on ELF `46d975be7d38f128` · **Severity:** was low-medium
(unbounded handler growth on a device with ~16 KB free) · **Verified** by code
2026-07-30, then **confirmed by a before/after measurement**

`EvilPortal::restartWiFi()` calls `webServer.end()` and then `setupRoutes()` again. But
**`end()` does not clear the handler list** — only `reset()` does
(`WebServer.cpp:199-206`, which does `_rewrites.clear(); _handlers.clear();`). And every
`webServer.on(...)` allocates a fresh `AsyncCallbackWebHandler` and appends it
(`:210-213` → `addHandler` → `_handlers.emplace_back`).

So each `/ssid` rename appends a complete second copy of the route table, and the copies
are never freed until the server is destroyed. `_attachHandler` takes the **first**
match while iterating (`WebServer.cpp:145-153`), so the stale duplicates shadow the new
ones. Behaviour stays correct — the route lambdas capture `this`, and `this` has not
changed — so the cost is memory and lookup time, not wrong answers.

That cost lands on the subsystem least able to absorb it: ISSUE-21 shows the portal has
roughly **one** page load of headroom.

**Deliberately not fixed in `cedad77f`.** The fix is small — `webServer.reset()` in place
of `end()`, since `setupRoutes()` re-registers `onNotFound` and would restore the
catch-all — but it changes teardown semantics on a live path, and folding it into an
already-large flash cycle would have made any regression un-attributable.

**Not yet quantified.** No run has counted handlers or measured the per-rename heap delta;
the claim is from the library source, not a measurement. Worth confirming before fixing.

**Measured and fixed 2026-07-30.** The fix is `webServer.reset()` immediately after
`webServer.end()` in `restartWiFi()` (`evil_portal.cpp:282`). It is safe because
`setupRoutes()` re-registers `onNotFound` (`:262`), which `reset()` nulls along with the
handler list.

| rename #1 | free before | free after | delta |
|---|---|---|---|
| ELF `e81b0c28f80e70dd`, run A | 16,499 | 13,735 | **−2,764** |
| ELF `e81b0c28f80e70dd`, run B | 16,231 | 13,375 | **−2,856** |
| ELF `46d975be7d38f128` (fixed) | 16,735 | 16,511 | **−224** |

Two independent before-measurements agree to within 92 bytes, and the fix removes ~2.6 KB
of it. That matches the predicted cost of duplicating ~17 `AsyncCallbackWebHandler`s at
roughly 160 bytes each, which is what makes this a confirmation rather than a coincidence.

⚠️ **Only ONE rename per run was ever measured, and the per-rename figure above is
gross, not a pure handler cost.** Two attempts to measure *repeated* renames both failed,
and the failure is worth recording because it will defeat the next attempt too:

- **Attempt 1** renamed to a fresh SSID each time. `evilportal -status` showed `ssid:Ren1`
  at the end, so **1 of 4 renames landed** — re-association failed once the AP changed
  name. Its "~468 B per rename" was an artifact and is discarded.
- **Attempt 2** renamed to the *same* SSID (the handler never compares, so this still
  forces a full restart) to keep the profile valid. The console showed only **one**
  `STA disconnect failed` line during the run — `restartWiFi()`'s signature — plus one at
  portal stop, so again **only rename #1 executed**. `nmcli` reported the profile still
  "active", which is why the harness's own liveness check did not catch it.

**The rename destroys the association needed to issue the next rename, and `/ssid` is the
only trigger** (`_pendingWifiRestart` is set nowhere else — `evil_portal.cpp:254`, consumed
at `:316-318` and `:409-411`). So "unbounded growth" remains **inferred from the library
source, not observed**; what is now measured is that one rename costs ~2.8 KB before the
fix and ~0.2 KB after.

---

### ISSUE-41 — `loopOptions()` reads a freed label after the handler returns, panicking the main loop

**Status:** **RESOLVED 2026-07-30 in `display.cpp`** — label copied before the call ·
**Severity:** was critical (crashes the main menu, the most-used path on the device) ·
**Verified** on hardware, ELF `2efaeec784a5768a` · **Reproduced 2/2, then 0/1 after the fix**

**A fork-introduced regression.** The `log_e("Returned from: %s", …)` line did not exist
upstream; it was added by this fork to give ISSUE-30 something to go on, and it reads a
label that the handler it just called may have destroyed.

`options` is the **global** at `include/globals.h:176`, and `loopOptions()` takes it
**by reference**. Many handlers rebuild that global — `evil_portal.cpp:71-73`, `:92-97`,
`:102-109` all do `options = {…}`. So the sequence was:

```cpp
log_e("%sSelected: %s", …, options[chosen].label.c_str());  // safe — before the call
options[chosen].operation();                                // handler reassigns `options`
log_e("Returned from: %s", options[chosen].label.c_str());  // reads a destroyed String
```

**Decoded backtrace**, innermost last. `ELF file SHA256: a4bc5d735` matches the flashed
`a4bc5d735f793762`, so this decode is authoritative:

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
EXCVADDR: 0x00000000   A2: 0x00000000        <- NULL %s

vPortTaskWrapper                        port.c:139
loopTask                                main.cpp:82
MainMenu::begin / loop()                main_menu.cpp:69, main.cpp:626
loopOptions                             display.cpp:754   <- "Returned from: %s"
__wrap_log_printf                       esp_diagnostics_log_hook.c:418
vsnprintf / _vsnprintf_r / _svfprintf_r newlib
??                                      ROM                <- null deref
```

**It crashed the device twice during the ISSUE-39 operator test** and was initially
mis-attributed to that session's `wifiDisconnect()` change. The backtrace refuted that:
the fault is on the **main loop task**, not the serial task, and has nothing to do with
the radio teardown. *Recorded because the wrong attribution was stated out loud before
the evidence existed.*

**Fix:** copy the label into a local before invoking the handler, and log the copy.

**Verified without an operator**, which is worth reusing — the `options <n>` verb sets
`forceMenuOption` (`util_commands.cpp:323`), so the whole crash path is drivable over BLE:

| step | result |
|---|---|
| `options 0` → force-select "WiFi" | `Forcely Selected: WiFi` |
| `optionsJSON` | now returns the **WiFi submenu**, 20 entries — proves the global was rebuilt under the live reference |
| `options 19` → force-select "Main Menu" | `Returned from: Main Menu`, then **`Returned from: WiFi`** — the exact instruction that panicked |
| uptime across the whole sequence | **00:00:51 → 00:02:15, continuous — no reset** |

⚠️ **Consequence for every operator test in this register.** While a blocking verb holds
the serial task, the **main loop keeps running the menu UI**, so physical button presses
drive *both* it and the blocked verb's own `check()`. During the ISSUE-39 test the
operator's LEFT+RIGHT went to the main menu — `loopOptions(): Selected: WiFi` is in the
console — rather than to `reverseshell`'s Esc check. This is ISSUE-19's race one module
along, and it means **an on-device chord is not a reliable way to exit a blocking verb.**

**Second trap found the same way:** `InputHandler()` swallows the first press whenever the
screen has dimmed — `if (!wakeUpScreen()) AnyKeyPress = true; else return;`
(`boards/smoochiee-board/interface.cpp:119-123`, `display.cpp:114-129`). `EscPress` is
never set on that press. **Any instruction that says "press LEFT+RIGHT to exit" is
incomplete: on a dimmed screen it takes two.**

---

### ISSUE-42 — the `options` verb's reply went to `Serial` and never reached the app

**Status:** **RESOLVED 2026-07-30 in `util_commands.cpp`** · **Severity:** low (`optionsJSON`
already covered the read path) · **Verified** over BLE, ELF `3dd17e72827f4325`

`optionsList()` wrote every line with `Serial.println` (`util_commands.cpp:238-247`) in a
file that uses `serialDevice->` **97 times**. This is the bug class BRUCELINK.md already
flags for `settings_commands.cpp:19` — output on `Serial` reaches nothing on this board
(ISSUE-22). Caught while driving the menu remotely for ISSUE-41: `options` returned
**3 bytes** (the bare prompt) against a 14-entry menu, while `optionsJSON` returned 442.

**The fix is a parameter, not a substitution, and that distinction matters.**
`optionsList()` has two callers with different needs, and `navCallback` carries an
explicit comment — *"Here send press response only to USB serial to avoid problems with
BLE app"* (`:265`). Switching the function wholesale would have dumped the entire menu onto
the BLE characteristic **after every `nav` pulse**, and ISSUE-19 establishes that `nav` is
pulsed repeatedly until the device answers. So the target is passed in: `optionsCallback`
passes `serialDevice`, `navCallback` passes `&USBserial`, and the documented intent
survives.

| check | before | after |
|---|---|---|
| `options` over BLE | **3 bytes** | **222 bytes**, full menu with the `>` hover marker |
| `nav next` / `nav prev` over BLE | 3 bytes | **3 bytes** — dump correctly stays off BLE |
| `options <n>` | selection only | selection **+ list** reaches the app |

**A second defect was fixed in the same function.** `optionsCallback` reported the choice
by reading back `options[forceMenuOption]` *after* assigning it — but `forceMenuOption` is
`volatile int` and the main loop clears it to `-1` the moment it consumes the selection
(`display.cpp:742-744`), so an unlucky interleave indexes `options[-1]`. It now reports
from the local `opt`. Never observed firing; found by reading the path. Same family as
ISSUE-41 — a global mutated by another task, read back after a hand-off.

---

## Not tested, and why

Recorded so the gap is visible rather than implied. Session of 2026-07-29, unattended.

| Item | Why not |
|---|---|
| ~~`POST /cm cmnd=nav esc` against a running blocking verb~~ | **DONE 2026-07-29** — see ISSUE-19. The rescue works on `ap_info` but needs repeated pulses, and is unavailable for every radio verb. |
| ~~Minimum pulse count for the `nav` rescue~~ | **DONE 2026-07-30 — there is no fixed minimum.** Single pulses 3 s apart against one blocked `ap_info`: #1, #2, #3 failed, #4 released. Against 2026-07-29's 1-failed/6-worked, the count is nondeterministic; a three-way race explains why. See ISSUE-19. **Do not bisect for a threshold.** |
| Whether `pwngrid` is rescuable | It is the only other blocking verb that appears to touch neither radio, so it should be, but it was not tested. |
| `/getscreen`, `/listfiles`, `/file`, `/upload`, `/edit`, `/rename`, WS `/ws` | Same blocker. `GET /` (200) and `GET /systeminfo` (401 unauth) are the only routes exercised. |
| ~~`badusb run_from_file`~~ | **DONE 2026-07-29, attended** — types nothing and hangs the device forever. See ISSUE-20. |
| ~~`badusb` **BLE HID** variant~~ | **NOT REACHABLE 2026-07-29** — not a testing gap. Both CLI callbacks hardcode the transport: `ducky_startKb(hid_usb, false)` at `badusb_commands.cpp:31` and `:64`, and the verb takes no `ble` argument (`run_from_file <filepath>`, `run_from_buffer`). `ducky_startKb(..., ble=true)` exists but only the on-device menu reaches it. Combined with ISSUE-20 this means **BadUSB is entirely unavailable to the companion app**: the USB branch cannot enumerate on this build, and the BLE branch is not wired to the command bus. Exposing it needs a new verb or an argument, not a test. |
| JS `print`/`println` | Untested and hazardous for the same reason as `badusb`: they are the badusb HID natives (`mqjs_stdlib.h`), not console output. |
| `wifi add` / `wifi on` / `wifi off` | The user chose the AP path for this session, which does not exercise them. Zero evidence either way. |
| ~~FastPair **handset** popup after `c9c43c03`~~ | **DONE 2026-07-29** — Android popup confirmed by the user, with 16 valid `0xFE2C` adverts captured concurrently. See §Resolved ISSUE-8. |
| ~~Evil Portal under load~~ | **DONE 2026-07-29** — it crashes, same assertion as `deauth`. Confirms the ISSUE-1 hypothesis; see ISSUE-1. |
| ~~Evil Portal capturing a real credential~~ | **DONE 2026-07-29 with `ble api off`** — `testvictim@example.com` / `NotARealPassword123` captured and returned at `/creds`. See §Resolved-in-place in ISSUE-21. **Not re-run since the headless verb landed**, and still impossible with the BLE API armed. |
| ~~Free heap during an active Evil Portal~~ | **DONE 2026-07-30** — 16,915 free / 8,180 dma at portal start with BLE armed, versus 80,951 / 31,732 idle. This confirmed the memory hypothesis in ISSUE-21. |
| **ISSUE-24's fix (`d71f19e9`) on hardware** | **Still unverified after three attempts** 2026-07-30 — one invalidated by an operator RST, one blocked by the ISSUE-12 memory ceiling, one designed wrong (device parked in a submenu, where the repaint provably cannot fire). The only discriminating test is `POST /cm cmnd=evilportal`; see ISSUE-24. |
| Credential capture with the **headless** portal | Approved but never run. Needs `ble api off` for the heap, plus a short duration cap so the portal self-stops — the cap is proven (+45.6 s on a 45 s cap) which is what makes the BLE-off configuration recoverable at all. |
| ISSUE-30's root cause | No backtrace obtainable — nothing panics, no watchdog fires. Next experiment is changing `display.cpp:744`'s `Serial.println` to `log_e` so the "Selected:" line becomes visible. |
| ~~Whether `172.0.0.1` really breaks handset captive-portal detection~~ | **DONE 2026-07-30, two real handsets — it does NOT.** iOS opened the captive sheet by itself, Android opened a browser by itself, both on the stock `172.0.0.1`. The code comment is false and the fix direction is settled (drop the dead branch). Both then showed a blank page, which is ISSUE-21, not addressing. See ISSUE-27. |
| `poweroff`, `sleep` | Would take the device down with nobody present to power-cycle it. `reboot` was tested instead and passed 2/2. |
| `blespam random`/`all`, interactive `blespam menu` | Menu-driven; needs on-device dismissal. |
| MTU 247 negotiation | BlueZ negotiates 128 and will not go higher; needs an Android client. Chunking remains half-verified. |
| `[TRUNCATED: device low on memory]` marker | Never observed — and notably **did not appear** in the one case that should have triggered it (ISSUE-16), where replies were silently cut instead. |
| ISSUE-17 leak vs. one-off allocation | Distinguishing them needs `free` sampled after each individual `js` run; only aggregate before/after was measured. |

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

**Handset confirmation, 2026-07-29 — the fix is now proven end-to-end.** The user
watched an Android handset during `blespam fastpair_regular 900` and reported the Fast
Pair popup appearing. The simultaneous packet capture recorded **16 distinct addresses
carrying 0xFE2C service data in 45 s**, cycling five model IDs (`000047`, `000048`,
`00000a`, `0000f0`, `000006`), each on a fresh random MAC, with company ID `0x0303`
absent entirely.

This retires the caveat this entry carried since the fix landed, that only the radio
level was proven. Note the earlier Android null result was **not** a firmware fault —
it was "Scan for nearby devices" being off on the handset, which is what made the
original diagnosis ambiguous.

**iPhones do not implement Fast Pair.** `0xFE2C` is a Google protocol; an iPhone
showing nothing for `fastpair_*` is correct behaviour, not a defect. The iOS popup
comes from `blespam apple` (Continuity NearbyAction), a different payload.

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

### ISSUE-27 — the `192.168.4.1` captive-portal gateway default never applies on a configured device

**Status:** RESOLVED in `cedad77f` · **Severity was:** low (dead code, not a functional
defect) · **Verified** 2026-07-30 · **Settled against two real handsets 2026-07-30** ·
**Fix proven on hardware** 2026-07-30

`evilportalCmdCallback` sets the phone-friendly gateway **only when the stored value is
empty** (`attack_commands.cpp:58-59`):

```cpp
if (bruceConfig.evilPortalGatewayIp.isEmpty()) {
    bruceConfig.evilPortalGatewayIp = "192.168.4.1";
}
```

But the config loader never leaves it empty — it defaults to **`172.0.0.1`**
(`config.cpp:332`). So on any device whose config has been written once, the branch
never fires and the portal comes up on `172.0.0.1`.

**The portal does come up on `172.0.0.1`** — a laptop associating to it is addressed out
of `172.0.0.0/24` and the portal answers there.

⚠️ **This entry previously said the laptop "received a lease with gateway `172.0.0.1`".
That was wrong** — a misreading of the lease. Re-measured 2026-07-30 by dumping the raw
DHCP options (`nmcli -f DHCP4 con show`): the client **requested** the routers option
(`requested_routers = 1`) and the server sent **no `routers` option at all**. What the
lease actually carries is `ip_address = 172.0.0.2`, `dhcp_server_identifier = 172.0.0.1`,
`domain_name_servers = 172.0.0.1`, `dhcp_lease_time = 7200`, `interface_mtu = 1500`. The
`172.0.0.1` in the earlier note was the DHCP server identity and the DNS address — never
a gateway. See ISSUE-36.

**The code argues against its own behaviour.** The comment directly above that branch
states that `172.0.0.1` breaks Android/iOS captive-portal auto-detection and that
phones expect `192.168.4.1`. If that is correct, then the compatibility it exists to
provide **is not in effect on this device, and never has been** — the default is
unreachable in practice.

**ANSWERED 2026-07-30 against two real handsets: `172.0.0.1` does NOT break
captive-portal auto-detection. The code comment is false.**

Portal `PortalTest` (open, ch 6) on the stock `172.0.0.1`, operator's own phones:

| Handset | What happened unprompted |
|---|---|
| iPhone / iOS | The **captive-portal sheet opened by itself**. Page then rendered blank white. |
| Android | A **browser opened by itself**. Page then failed to load. |

Both platforms *detected* the portal without any user action, which is the entire claim
the comment makes against `172.0.0.1`. The blank/failed page is a **separate and much
worse defect** — the portal runs out of heap after a single page load, see ISSUE-21,
which this test quantified. It is not a gateway-addressing problem.

The detection path was measured directly and explains why the address is irrelevant:
DNS is hijacked (`dig @172.0.0.1 connectivitycheck.gstatic.com` → `172.0.0.1`, likewise
`captive.apple.com`), and both probes return the "portal present" answer —
`/generate_204` → **302** (not 204), `/hotspot-detect.html` → **200** with a
meta-refresh (not the `Success` body iOS wants). The probe target resolves onto the
AP's own `/24`, so it is reachable **without any default route** — which is why the
missing routers option (ISSUE-36) does not break detection either.

**Fix direction — settled: drop the dead `isEmpty()` branch** at
`attack_commands.cpp:58-60` and the misleading comment above it. Do **not** force
`192.168.4.1`; there is no measured benefit to it, and the branch as written can never
execute anyway. Note also that `evilPortalGatewayIp` is **not reachable from the
companion app at all** — it is absent from the `settings` verb's field list
(`settings_commands.cpp:106-131`) and can only be changed from the on-device menu
(`settings.cpp:611-620`), so no remote caller can select either value.

**Left untested on purpose:** whether `192.168.4.1` behaves *better*. It cannot matter
for the fix now chosen, and it would cost an operator-attended reflash-free menu change
plus two more handset runs to learn nothing actionable.

**FIXED in `cedad77f`** — the `isEmpty()` branch and its false comment are gone from
`attack_commands.cpp`. **Proven on hardware 2026-07-30** (ELF `dafff2ebcbd41cb2`): the
point of the fix is that behaviour is *unchanged*, and it is. A background portal still
comes up on `172.0.0.1`, a laptop still associates and is addressed `172.0.0.5/24`, and
`nmcli` still reports `IP4.GATEWAY: --` (ISSUE-36, untouched). Nothing was forced to
`192.168.4.1`, which is exactly the intent.

---

### ISSUE-32 — `CaptiveRequestHandler` is inert: a `const` mismatch means it never overrides

**Status:** RESOLVED in `cedad77f` · **Severity was:** low (behaviour is nearly identical;
it was dead weight and a trap) · **Verified** by code 2026-07-30 · **Fix proven on
hardware 2026-07-30**

`CaptiveRequestHandler::canHandle` is declared **non-const** (`evil_portal.h:14`):

```cpp
bool canHandle(AsyncWebServerRequest *request) { return true; }
```

The base declares it **const** (`ESPAsyncWebServer.h:1533`):

```cpp
virtual bool canHandle(AsyncWebServerRequest *request) const { return false; }
```

Different signatures, so this **does not override** — there is no `override` keyword to
have caught it. Dispatch calls `h->canHandle(request)` through the base pointer
(`WebServer.cpp:147`), gets the base's `false`, and **skips the handler for every
request**. Consequences:

- `CaptiveRequestHandler::handleRequest` (`evil_portal.cpp:54-78`) **never runs**, so the
  `AsyncResponseStream` it allocates at `:55` and never passes to `request->send()` — a
  genuine per-request leak of a 1,460-byte `cbuf` plus object, by construction — **never
  actually fires.** Recorded because fixing the `const` bug without also deleting line 55
  would *introduce* that leak.
- `webServer.onNotFound(...)` (`:278-294`) is therefore the **live** fallback, not dead
  code, and it duplicates most of `handleRequest`'s logic.
- The handler object `new`ed at `:296` is pure overhead.

**Established by measurement, not just reading:** heap returned fully to its normal
post-portal plateau after sessions containing many requests (53,143 vs the 52,231-52,991
plateau), which a live 1.4 KB-per-request leak could not do.

**Fix direction:** delete the class and the `addHandler` call, keeping `onNotFound`. Do
*not* merely add `const`.

**FIXED in `cedad77f`** — the class (`evil_portal.h`), its `handleRequest` and the
`addHandler`/`setFilter` call are deleted; `onNotFound` and the `_captiveHandler` member
went with them. The `const` was deliberately *not* added, so the dormant
`beginResponseStream` leak was never activated.

**Proven behaviour-neutral on hardware 2026-07-30** (ELF `dafff2ebcbd41cb2`), which is
the whole claim — a handler that never ran cannot be missed. Captive-portal routing after
the deletion, measured from a laptop on the portal AP:

| Request | Result |
|---|---|
| `GET /generate_204` | **302** → `http://172.0.0.1/` |
| `GET /hotspot-detect.html` | **200**, 99 bytes (meta-refresh) |
| `GET /canonical.html` | **302** → `http://172.0.0.1/` |

Identical to the pre-deletion measurements recorded under ISSUE-27. Heap also returned to
the normal post-portal plateau across three portal cycles (**53,563 / 53,303 / 53,175**
against the documented 52,231-53,143), so the deletion introduced no leak of its own.

---

### ISSUE-33 — `evilportal -status` reports a portal whose AP is dead as "running"

**Status:** RESOLVED in `cedad77f` · **Severity was:** medium (the app showed a healthy
portal that captured nothing) · **Verified** on hardware 2026-07-30 · **Fix partly proven
on hardware 2026-07-30 — see the caveat below**

`evilPortalBgStatus()` (`evil_portal_bg.cpp:115-124`) keys everything off
`bgPortal != nullptr` and never consults `apOnAir()` or `_servicesUp`. Measured
immediately after the induced softAP failure of ISSUE-28, with the AP provably gone from
an `nmcli` scan and HTTP refusing connections:

```
portal: running ssid:PortalTest ch:6 uptime_s:126 creds:0 cap_remaining_s:473 …
```

The portal was serving nothing and had no AP, yet reports a live cap and a running state.
`restartWiFi()` sets `_servicesUp = false` on that path (`:314`) — the information exists
and is simply not surfaced.

**Fix direction:** have the status line carry `apOnAir()`/`_servicesUp`, and consider
having the tick stop a portal that has lost its AP rather than leaving a zombie.

**FIXED in `cedad77f`.** A `servicesUp()` accessor was added beside `apOnAir()`, and the
status line now leads with a real state word plus both flags:

```
portal: running ap:up services:up ssid:PortalTest ch:6 uptime_s:51 creds:0 …
```

It reads `degraded` when either flag is false. The tick was **not** changed to auto-stop a
zombie portal — that alters recovery semantics and belongs in its own change.

**A second lie in the same line was found and fixed while verifying this one.** The status
and the stop message printed `bgSsid`, captured at portal start, so both were wrong the
moment `/ssid` renamed the AP. Caught live on ELF `dafff2ebcbd41cb2`: the AP was
broadcasting `BBBB…` (32 chars, confirmed by `nmcli` scan) while the device reported
`ssid:PortalTest`, and `evilportal -off` printed `portal 'PortalTest' stopped`. Both now
read `bgPortal->getApName()`. Re-verified on ELF `f5244eb35dd10795`: renaming to
`CleanName` gave `… ssid:CleanName …` and `portal 'CleanName' stopped.`

⚠️ **The `degraded` branch is code-verified only, NOT hardware-verified — and it can no
longer be induced.** The only known way to kill a running portal's AP was
`GET /ssid?ssid=` with an empty value, and **ISSUE-34's fix in this same commit now
rejects that with 400**. A 40-char SSID was previously measured *not* to fail, and 33+
bytes is now rejected too. So the `running`/`ap:up` path is proven and the `degraded` path
is not. Inducing it again needs a softAP failure reachable some other way.

---

### ISSUE-34 — any client on the portal AP can kill the portal with one unauthenticated request

**Status:** RESOLVED in `cedad77f` · **Severity was:** medium (remote self-DoS of a
running attack) · **Verified** on hardware 2026-07-30 · **Fix proven on hardware
2026-07-30**

The `/ssid` route assigns `apName` from the query argument with **no validation of length
or emptiness** and no authentication (`evil_portal.cpp:267-270`, and the identical
unreachable copy at `:67-70`):

```cpp
if (request->hasArg("ssid")) {
    apName = request->arg("ssid").c_str();
    request->send(200, "text/html", ssid_POST());
    _pendingWifiRestart = true;
}
```

`GET /ssid?ssid=` — an empty value — therefore drives `WiFi.softAP("")`, which fails, and
the AP goes down permanently for that portal instance. Confirmed live; it is the induction
used to verify ISSUE-28. A victim, or anyone in range of an open AP, can end the attack.
Enabled by default on this device (`allowSetSsid: true`, read from `/bruce.conf`).

**Fix direction:** reject an empty or >32-byte SSID before assigning, and leave the
running AP untouched when the value is invalid.

**FIXED in `cedad77f`** — the live route validates before assigning and returns `400
invalid ssid` without touching `apName` or setting `_pendingWifiRestart`. (The identical
copy at `:67-70` needed no fix: it lived inside `CaptiveRequestHandler`, deleted in the
same commit under ISSUE-32.)

**Proven on hardware 2026-07-30** (ELF `dafff2ebcbd41cb2`), laptop on the portal AP:

| Request | Result |
|---|---|
| `GET /ssid?ssid=` (empty — *the request that used to kill the AP*) | **400** `invalid ssid`, 12 B, **11.8 ms** |
| `GET /ssid?ssid=` + 33 bytes | **400** `invalid ssid`, 12 B, **4.7 ms** |
| `GET /ssid?ssid=` + 32 bytes | **accepted** — AP renamed, confirmed by independent `nmcli` scan |

The boundary is correct: 32 is a legal 802.11 SSID and is allowed, 33 is not. Decisively,
the 33-byte request **succeeded after the empty one**, and an independent scan still found
the AP (ch 6, signal 81) — on the previous build the empty request took the AP down for
good. `evilportal -status` still reported `running ap:up services:up` throughout.

**Note on the accepted path:** a valid rename returns `http=000` to the client, because
`request->send()` is asynchronous and the `_pendingWifiRestart` teardown drops the
connection before the response flushes. That is pre-existing upstream behaviour, not
introduced here — the rename itself demonstrably works.

---

### ISSUE-35 — `shouldTerminate()` is the only consumer of a duration policy `karma` actively maintains

**Status:** RESOLVED in `cedad77f` · **Severity was:** medium (karma believed it had a
portal time limit and had none) · **Verified** by code 2026-07-30

ISSUE-31 notes `shouldTerminate()` is uncalled. It is worse than an unused helper: it is
the **sole reader** of `_baseDurationSec` and `_extendedDurationSec`
(`evil_portal.cpp:462-471`), and `karma` populates and maintains exactly those:

| Site | Call |
|---|---|
| `karma_attack.cpp:1792` | `setBaseDuration(attackConfig.baseDuration / 1000)` |
| `karma_attack.cpp:1793` | `setExtendedDuration(attackConfig.extendedDuration / 1000)` |
| `karma_attack.cpp:1709` | `checkAndExtendDuration()` — flips `_durationExtended` |

A tree-wide grep finds `shouldTerminate` **only** at its declaration (`evil_portal.h:59`)
and definition. So karma configures a base duration, extends it on activity, and
**nothing ever checks whether the duration elapsed**. The whole policy is inert.

This is also why the headless portal had to implement its own cap in `portal_cap.h`
rather than reuse this.

**Fix direction:** either call `shouldTerminate()` from `loop()`/`processRequests()` —
which would also give the blocking verb the time-based exit ISSUE-31 identifies as
missing — or delete the subsystem and karma's three calls into it. Do not leave it
half-wired.

**FIXED in `cedad77f` by DELETING the subsystem.** Removed: `shouldTerminate`,
`checkAndExtendDuration`, `hasRecentActivity`, `setBaseDuration`, `setExtendedDuration`,
and the members `_baseDurationSec`, `_extendedDurationSec`, `_lastActivityTime`,
`_durationExtended`, `_launchTime` — plus karma's three call sites. `hasRecentPageView()`,
`recordPageView()` and `_lastPageViewTime` were **kept**: karma genuinely uses them at
`karma_attack.cpp:1721`.

**Why deleting beat wiring it up**, against the fix direction's first option — wiring it
in would have regressed two live paths:

- **`loop()`** (blocking portal): nothing calls `setBaseDuration()` on it, so it would
  have inherited the `_baseDurationSec = 15` default and **self-exited after 15 seconds**,
  destroying the interactive verb.
- **`processRequests()`** (background portal): `evil_portal_bg.cpp` already owns the cap
  via `portalCapExpired()` and honours the operator's `-duration`, including `0` for
  unlimited. The same 15 s default would have **silently overridden every `-duration`**.

And the policy is not actually missing anywhere: karma implements it **inline and
correctly** at `karma_attack.cpp:1711-1739`, against its own `activePortal->launchTime`,
with a 15 s idle timeout, no timeout while `hasRecentPageView()` is true, and a 180 s
absolute cap. The deleted EvilPortal-side copy was a second, never-consulted
implementation of the same idea.

**Verification is compile-and-link only, and that is the honest bar for a deletion**:
`pio run -e smoochiee-board` SUCCESS and `pio test -e native` 21/21 with every reference
gone (grep-confirmed across `src/` and `include/`). Karma's own timeout path was **not**
exercised on hardware — it needs live target handsets to drive `launchTime`, and no karma
run was performed this session.

---

### ISSUE-38 — `reverseshell` corrupts the heap and reboots on every clean exit

**Status:** RESOLVED in `cedad77f` · **Severity was:** **high** (guaranteed crash; it was
the verb's *only* exit path) · **Verified** on hardware 2026-07-30 · **Root cause
established, backtrace ELF-matched** · **Fix proven on hardware 2026-07-30**

The verb now works — see below — but **ending it always crashes the device.** The operator
pressed the documented LEFT+RIGHT Esc chord and the device rebooted. Console:

```
CORRUPT HEAP: Bad head at 0x3fcd7594. Expected 0xabba1234 got 0x00000002
assert failed: multi_heap_free multi_heap_poisoning.c:279 (head != NULL)
ELF file SHA256: 411d7e151          <- matches the local ELF, decode is trustworthy
rst:0xc (RTC_SW_CPU_RST)
```

Decoded, the chain is unambiguous:

```
_serialCmdsTaskLoop → SerialCli::parse → reverseshellCmdCallback (attack_commands.cpp:249)
  → ReverseShell() at reverseShell.cpp:211        <- function returning, locals destroyed
    → AsyncWebServer::~AsyncWebServer() (WebServer.cpp:61) → reset() (:202)
      → list<unique_ptr<AsyncWebHandler>>::clear() → default_delete
        → operator delete(void*)                  <- on a STACK address
```

**Root cause: a stack object is handed to a container that owns it.**

- `AsyncWebSocket ws("/ws");` is a **function-local** at `reverseShell.cpp:13`.
- `webServer.addHandler(&ws)` at `:107` passes its address to
  `AsyncWebServer::addHandler`, which does `_handlers.emplace_back(handler)` into a
  `std::list<std::unique_ptr<AsyncWebHandler>>` (`WebServer.cpp:96-99`) — **unconditional
  ownership**, plain `default_delete`, no `_freeOnRemoval` consultation.
- So `~AsyncWebServer()` calls `delete` on memory that was never `new`ed.

**It is also a use-after-destruction.** `ws` is declared *after* `webServer` (`:13` vs
`:12`), so reverse-declaration-order destruction destroys `ws` **first**, and the server
then deletes the already-destructed object.

This fires on **every** exit from the loop's `break` (`:200-207`), so there is no
non-crashing way to end the verb. It is a **different class from ISSUE-1** — heap
corruption from bogus ownership, not the SPI-bus mutex assert — and it is not load- or
timing-dependent.

**Contrast that proves the pattern:** the Evil Portal does the same call correctly —
`_captiveHandler = new CaptiveRequestHandler(this); webServer.addHandler(_captiveHandler);`
(`evil_portal.cpp:296-297`) — heap-allocated, so its teardown is safe. That is why the
portal does not crash on `evilportal -off`.

**Fix direction:** heap-allocate the websocket and let the server own it, matching the
portal — `auto *ws = new AsyncWebSocket("/ws"); webServer.addHandler(ws);` — and do not
delete it by hand. Do **not** simply reorder the declarations; that fixes the
use-after-destruction but leaves `delete` being called on a stack address.

**What does work, verified the same run** (clean boot, free 81,099 / dma 31,732):

| Check | Result |
|---|---|
| AP `BruceShell` on air | **yes** — WPA2, ch 1, independent `nmcli` scan |
| DHCP lease | **192.168.4.2/24** (gateway hardcoded `192.168.4.1`, `reverseShell.cpp:15` — *not* the config value) |
| Web UI, port 80 | **200, 3,459 bytes** |
| TCP listener, port 23 | accepts, sends all three banner lines |
| `/ws` → target relay | target received `whoami-probe` |
| target → `/ws` relay | `relay-proof-8421` returned verbatim |

So the WPA2 passphrase fix is proven: the AP that could never start on any prior build now
starts, and the relay works both directions.

**Read the architecture before testing it — the device is the C2, not the shell.** Port 23
is where the *target* dials in; the *operator* drives it from the browser WebSocket at
`/ws` (`reverseShell.cpp:13`). `WS_EVT_DATA` (`:34-56`) pushes the command to `tcpClient`
and reads its output back with a 3 s budget, breaking early only on a prompt ending
`"\n> "`, `"\n$ "` or `"\n# "` (`:48`). Connecting to port 23 and typing gets no response,
because nothing reads the operator side there — that is by design, not a defect.

**No remote rescue exists.** `reverseshell` binds port 80 itself, so the Bruce WebUI and
its `POST /cm cmnd=nav esc` are gone for the verb's whole life. Another instance of
ISSUE-19's radio-verb rule. **This is also why verifying the fix required an operator at
the board** — the exit is reachable only by the physical LEFT+RIGHT chord.

**FIXED in `cedad77f`.** `AsyncWebSocket` is now heap-allocated with
`new (std::nothrow)` and handed to `addHandler()`, which owns it; it is never deleted by
hand. It is allocated **after** the AP is up, so the two early `return false` paths above
it (`softAPConfig` / `softAP` failure) cannot leak it, and a null allocation is reported
and bails out rather than dereferencing.

The root cause was re-confirmed against the actual library source in this build before
changing anything, not just from the backtrace: `addHandler` is
`_handlers.emplace_back(handler)` into `std::list<std::unique_ptr<AsyncWebHandler>>`
(`WebServer.cpp:96-99`), and `~AsyncWebServer()` calls `reset()`, which does
`_handlers.clear()` (`:199-206`).

**Proven on hardware 2026-07-30**, ELF `f5244eb35dd10795`, operator at the board.
`reverseshell` dispatched over BLE, `BruceShell` confirmed on air (ch 1, WPA2, independent
`nmcli` scan), then the LEFT+RIGHT Esc chord pressed:

| Check | Before (`411d7e151dbc2356`) | After (`f5244eb35dd10795`) |
|---|---|---|
| `CORRUPT HEAP: Bad head` | printed | **absent** |
| `assert failed: multi_heap_free` | printed | **absent** |
| `rst:0xc (RTC_SW_CPU_RST)` + boot banner | printed | **absent** |
| Uptime after the press | reset to ~0 | **`00:00:21:06`, continuous** |
| Serial task after the press | dead (rebooting) | free — BLE answered `uptime` in **122 ms** |

The **uptime continuity is the decisive evidence**: `t=1,266,545 ms` unbroken across the
press means the device never reset. The console captured on `/dev/ttyACM0` throughout did
not grow by a single line during the exit. `ReverseShell()` returned normally through the
`break` at `:200-207` — precisely the path that previously called `delete` on a stack
address.

**A separate defect was exposed by the working exit: the verb leaks its AP.** See
**ISSUE-39**. That is a pre-existing bug the crash was masking — until now nothing ever
survived the exit long enough to observe what it left behind.

---

