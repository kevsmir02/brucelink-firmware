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

---

### ISSUE-13 — `encrypt` then `decrypt` fails ~62% of the time, silently

**Status:** OPEN · **Severity:** high (silent data loss to the user's eye) ·
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

---

### ISSUE-14 — `settings <field> <value>` silently does nothing for most fields

**Status:** OPEN · **Severity:** high (writes report success and change nothing) ·
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

---

### ISSUE-15 — the JS interpreter runs scripts but has no return channel

**Status:** OPEN · **Severity:** high (no output, no errors, no result) ·
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

---

### ISSUE-17 — the JS interpreter permanently retains ~18 KB of internal heap

**Status:** OPEN · **Severity:** high (it is enough to break `webui`, see ISSUE-12) ·
**Verified** 2026-07-29 · **Measured across a reboot boundary**

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

### ISSUE-18 — `POST /login` writes the whole config to flash, and can abort the device

**Status:** OPEN · **Severity:** critical (it is the first request any client makes) ·
**Verified** 2026-07-29 · **Crash 1/1 with an ELF-matched backtrace; HTTP failure 2/2**

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

## Not tested, and why

Recorded so the gap is visible rather than implied. Session of 2026-07-29, unattended.

| Item | Why not |
|---|---|
| ~~`POST /cm cmnd=nav esc` against a running blocking verb~~ | **DONE 2026-07-29** — see ISSUE-19. The rescue works on `ap_info` but needs repeated pulses, and is unavailable for every radio verb. |
| Minimum pulse count for the `nav` rescue | 1 failed, 6 succeeded, with other attempts in between; the exact threshold was not bisected. Would need a fresh block per trial. |
| Whether `pwngrid` is rescuable | It is the only other blocking verb that appears to touch neither radio, so it should be, but it was not tested. |
| `/getscreen`, `/listfiles`, `/file`, `/upload`, `/edit`, `/rename`, WS `/ws` | Same blocker. `GET /` (200) and `GET /systeminfo` (401 unauth) are the only routes exercised. |
| `badusb run_from_file` / `run_from_buffer`, BLE HID variant | **Deliberately skipped.** It emits real USB HID keystrokes into the attached host — here, the laptop running the test session, whose focused window is a terminal. Unsafe unattended; needs an attended session with a scratch window focused. Note `print`/`println` in JS are the *badusb* natives (`mqjs_stdlib.h`), so JS payloads carry the same hazard. |
| `wifi add` / `wifi on` / `wifi off` | The user chose the AP path for this session, which does not exercise them. Zero evidence either way. |
| ~~FastPair **handset** popup after `c9c43c03`~~ | **DONE 2026-07-29** — Android popup confirmed by the user, with 16 valid `0xFE2C` adverts captured concurrently. See §Resolved ISSUE-8. |
| Evil Portal capturing a real credential, and under load | Needs a phone associating and typing. Still only ever run idle (ISSUE-1 predicts load is the real crash risk). |
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
