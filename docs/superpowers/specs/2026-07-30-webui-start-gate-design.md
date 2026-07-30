# RC3 — a memory gate and a real diagnostic sink for the WebUI start path — design

**Date:** 2026-07-30 · **Baseline:** HEAD `4cf44f36`, tree clean, `pio run -e
smoochiee-board` SUCCESS, `pio test -e native` 28/28. Device `1C:DB:D4:5E:D7:39` on
`/dev/ttyACM0` running ELF `fabcc0003` = this tree, coredump partition empty.

One change: make `webui` refuse to start when it cannot, admit it when it fails anyway,
and say so on a channel that exists.

Today it does none of the three. `startWebUi()` (`webInterface.cpp:752-802`) has no
memory gate, sets `isWebUIActive = true` unconditionally at `:777`, returns `void` so
its caller cannot report an outcome, and has **no failure detection at all** — so there
is not even a wrong diagnostic to redirect.

---

## Corrections this design rests on

Three claims in the handoff and the register are wrong. They are corrected here because
the design depends on the corrected versions.

### 1. `webInterface.cpp:767` is not a failure report

The handoff describes `startWebUi()` as reporting "failure only via `Serial.println` at
`:767`". `:767` is `Serial.println("Configuring Webserver ...")` — a **progress** line.
There is no failure report anywhere in `startWebUi()`, because nothing in it detects
failure. The nearest Serial-only *failure* text on the path is `startMdnsResponder()`'s
at `:399`.

The defect is therefore **worse** than described, not milder: this is not a misrouted
diagnostic, it is an absent one.

### 2. ISSUE-19's missing-log-line evidence is invalid, not weak

ISSUE-19 keeps the `blesniffer`-tore-down-the-AP attribution at **SUSPECTED** partly
because "**neither** its `[RAM] Low contiguous DMA memory…` line **nor**
`displayError("Low RAM: free WiFi/SD first")` appears in the capture", hedged with "the
USB console has demonstrably dropped output under memory pressure this session, so the
missing log line is weak evidence either way."

Both lines were **unobservable by construction**:

- `radio_mem.h:48,62,67` use `Serial.printf`/`Serial.println`. Per
  `ram_profile.cpp:9-18`, on `ARDUINO_USB_CDC_ON_BOOT` boards — smoochiee-board among
  them — `Serial` is the **native USB-CDC port**, while the bench laptop is attached to
  the **UART bridge**, which carries the ESP_LOG/UART0 stream. `RAM_LOG` is visible only
  because it explicitly mirrors to UART0 TX on GPIO43 (`ram_profile.cpp:19-32`). This is
  the mechanism behind ISSUE-22.
- `displayError` draws a red stripe on the **TFT**; it would never appear in a serial
  capture regardless. Its console half is also `Serial` (`display.cpp:317`).

So their absence was never evidence of anything. The hedge understates the problem: it
is not that the console *might* have dropped the lines, it is that the lines were never
addressed to the console at all. Converting them is part of this change, which makes the
next ISSUE-19 repro decisive rather than suggestive.

### 3. The contract's `log`-source claim is false

`docs/bruce-companion-api.md:347` reads: "That is the only `log` source; general CLI
stdout is **not** forwarded to `/ws`." There are six other producers today —
`evil_portal_bg.cpp:37,102,146,150`, `globals_js.cpp:287`, `serial_js.cpp:54` — and
`:146` already emits level `warn`. Drift in the **pessimistic** direction again, the
same direction last session found in the crash docs.

`level:'err'` is declared in the union at `:315` and has **zero** producers in the
firmware. RC3 will be the first.

---

## Why

### The margin is real and the failure is silent

ISSUE-12 reproduced both outcomes with console captures. From a fresh boot the WebUI
works — AP accepts a station, DHCP leases, HTTP serves, and `systeminfo` still answers
over BLE at `free_heap:14140`. From a boot where a single `js` run had already taken
~18 KB (ISSUE-17), the same command starts nothing: `[E][AsyncTCP.cpp:1521] begin():
failed to start task` with 1,235 bytes free, the AP beacons but cannot complete an
association, and BLE replies truncate to 1 byte.

`webui -bg` prints `AP` / `Press ESC to quit` and returns success in **both** cases. The
only in-band signal of failure is that nothing subsequently answers on port 80.

### There is an exact detector, not just a threshold

`AsyncWebServer::state()` is **public** (`ESPAsyncWebServer.h:1686-1693`) and forwards to
`AsyncServer::status()` (`AsyncTCP.cpp:1629-1633`), which returns `_pcb ? _pcb->state :
0`. A failed `begin()` returns early leaving `_pcb == 0` (`AsyncTCP.cpp:1515-1535`), and
lwIP's `LISTEN == 1` (`tcpbase.h:56-68`).

`state() != LISTEN` is therefore a **fact about whether port 80 is listening**, not an
inference from a heap figure. That is what lets this change assert something instead of
guessing.

### A pre-flight gate alone would miss one of the two documented failures

`RAM_LOG`'s `dma=` field is `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)`
(`ram_profile.cpp:45`) — the identical call to `radioLargestDmaBlock()`
(`radio_mem.h:25`). ISSUE-12's numbers therefore compare directly against
`RADIO_WIFI_MIN_DMA_BLOCK` = 15,360 (`radio_mem.h:29`):

| Run | `webui pre-alloc` dma | ≥ 15,360? | Outcome |
|---|---|---|---|
| 2026-07-29 working | 20,468 | yes | worked |
| 2026-07-29 after a `js` run | **1,844** | **no** | failed |
| 2026-07-30 run 1 | 19,444 | yes | worked |
| 2026-07-30 run 2 | **18,420** | **yes** | **failed** |

A 15,360 gate catches the catastrophic case and misses the marginal one. **The response
is not to invent a threshold between 18,420 and 19,444** — that boundary rests on one
observation each side, and inventing a number from it is precisely what this repo's
quality bar forbids. The response is to add the exact post-hoc check, and to be explicit
about what remains uncovered (see *Honest limits*).

### A third site has ISSUE-28's shape, on the path `webui` actually walks

`_setupAP()` (`wifi_common.cpp:142-150`):

```cpp
bool _setupAP() {
    IPAddress AP_GATEWAY(172, 0, 0, 1);
    WiFi.softAPConfig(AP_GATEWAY, AP_GATEWAY, IPAddress(255, 255, 255, 0));
    WiFi.softAP(bruceConfig.wifiAp.ssid, bruceConfig.wifiAp.pwd, 6, 0, 4, false);
    wifiIP = WiFi.softAPIP().toString();
    Serial.println("IP: " + wifiIP);
    wifiConnected = true;      // set regardless
    return true;               // unconditional
}
```

`WiFi.softAP()`'s return is discarded entirely. This is ISSUE-28's defect verbatim, one
module along.

And `wifiConnectMenu`'s `WIFI_AP` case (`wifi_common.cpp:182-185`) has **no memory
gate** — all three `radioHasMemForWifi()` call sites (`:189`, `:293`, `:348`) are STA
paths. `webui` defaults to AP mode (`wifi_commands.cpp:79`, `startWebUi(!noAp, …)` with
`noAp` unset). So the mode the verb actually uses is ungated at every level.

`startWebUi` also **discards** `wifiConnectMenu`'s `bool` (`:754-759`), so even the
existing STA gate cannot reach the caller.

---

## Design

### Four decision points

| # | Where | Test | On failure |
|---|---|---|---|
| **A** | `wifiConnectMenu()` `WIFI_AP` case, `wifi_common.cpp:182-185` | `radioHasMemForWifi()` | return `false`; nothing allocated, nothing to unwind |
| **B** | `_setupAP()`, `wifi_common.cpp:142-150` | `WiFi.softAP()`'s return | return `false` **without** `wifiConnected = true` |
| **C** | `startWebUi()` at `:768`, where `RAM_LOG("webui pre-alloc")` already sits | `radioHasMemForWifi()` | `wifiDisconnect()` if `!keepWifiConnected`; return `false` |
| **D** | `startWebUi()` immediately after `server->begin()` | `server->state() == LISTEN` | `stopWebUi()`, then `wifiDisconnect()` if `!keepWifiConnected`; return `false` |

**Gate A goes in `wifiConnectMenu`, not in `startWebUi`.** It mirrors the existing STA
gate at `:189`, keeps one threshold in one place, and covers the other AP caller
(`WifiMenu.cpp:53`) for free. `startWebUi` then simply honours the `bool` it currently
throws away.

**Gate C samples at `:768` deliberately** — the exact point ISSUE-12's `webui pre-alloc`
figures were taken, so the gate is directly comparable to the register's table. Sampling
at function entry instead would measure a pre-AP state that the register never recorded
and that was plentiful in both failing runs.

**Threshold: reuse `RADIO_WIFI_MIN_DMA_BLOCK` (15,360) unchanged.** No new constant.

### `isWebUIActive` moves behind gate D

This is the core of the fix. `isWebUIActive = true` moves from `:777` to after gate D
passes, so `display.cpp:985` and `loopOptionsWebUi()` (`:96`) stop believing in a server
that does not exist. `tft.setLogging()` (`:779`) and `drawWebUiScreen(mode_ap)` (`:780`)
become unreachable on every failure path.

`startWebUi` gains a `bool` return. `lambdaHelper` already discards it —
`return [=]() { (void)callback(args...); };` (`globals.h:178-181`) — so all four call
sites compile unchanged: `webInterface.cpp:100,107,108` and `startup_app.cpp:44`.
`webuiCallback` (`wifi_commands.cpp:55-82`) returns it as the verb's result, which makes
`[CLI] Result: FALSE` mean something here for the first time.

### Unwind ledger

`startWebUi` already tracks `keepWifiConnected` (`:753`, set at `:758`) — true when WiFi
was already connected on entry. It becomes the unwind ledger: gates C and D call
`wifiDisconnect()` **only** when `!keepWifiConnected`, so `webui` never tears down a
connection it did not create, and never leaves an AP broadcasting after refusing.

That second half is ISSUE-39's and ISSUE-31's lesson applied before it becomes a defect:
both entries exist because a verb exited cleanly while leaving its AP on air, and in
both cases `free` failing to return to its idle plateau was the whole tell.

Gate D reuses the existing `stopWebUi()` (`:47-59`), which already handles
`endWsServer()`, `server->end()`, the destructor, `free()` and `MDNS.end()`. Its
`isWebUIActive = false` and `tft.setLogging(false)` are harmless when the flag was never
set. `stopWebUi()` dereferences `server` with no null check at `:51`, which is safe here
and only here — gate D is the one path where `server` is guaranteed non-null.

Gates A and B allocate nothing, so they need no unwind at all.

### The pure module — `src/core/wifi/webui_gate.h`

Header-only, `std::string`, `snprintf`, free of Arduino, FreeRTOS and ESP-IDF so it
builds under `[env:native]` — copying `crash_report.h`'s established shape
(`crash_report.h:8-10`). `[env:native]` already passes `-I src`, so a test includes it
as `"core/wifi/webui_gate.h"`.

It holds:

- a `WebUiStartResult` enum — `Started`, `RefusedLowDmaPreWifi`, `RefusedApFailed`,
  `RefusedLowDmaPreAlloc`, `FailedNotListening`;
- a `WebUiStartReport` view struct carrying the DMA block at the deciding moment, the
  requirement, the `tcp_state` as read, and whether AP mode was requested;
- `webUiListening(uint8_t tcpState)` — `tcpState == 1`, with the lwIP value named in a
  comment rather than an include, keeping the header IDF-free;
- `webUiDmaSufficient(uint32_t block, uint32_t required)`;
- `webUiResultSlug(WebUiStartResult)` — stable machine-readable tokens;
- `formatWebUiStartReport(const WebUiStartReport &)` — the one human line.

Output is **one line, field per token**, for the same reason `crash_report.h:39-40` gives:
this is read over BLE where a reply can truncate (ISSUE-16) and a half-received line
must still be useful.

### The diagnostic sink — one helper, four destinations

A single `static void reportWebUiStart(const WebUiStartReport &)` in
`webInterface.cpp`, called on every outcome including success:

| Destination | Why this one |
|---|---|
| `log_e("%s", line)` | The only console path that works. `CORE_DEBUG_LEVEL=1` (`boards/smoochiee-board/smoochiee-board.ini:21`) compiles out everything below ERROR, so `log_e` is the only level available — not a stylistic choice. Proven to reach this board's console by ISSUE-28's live capture (`[E][evil_portal.cpp:313]`). |
| `pushWsLog(line, err ? "err" : "info")` | Both transports, via `pushWsEvent()`'s existing fan-out. Already declared in the contract at `:315`; RC3 is its first producer. `webInterface.cpp:12` already includes `ws_events.h`. |
| `serialDevice->println(line)` | The CLI reply, so `webui` over BLE says **why**. Never `Serial` — ISSUE-22, and ISSUE-42 was exactly this bug. |
| `displayError(line, false)` | The on-device operator. |

**`displayError`'s second argument must be `false`.** With `waitKeyPress = true` it spins
on `while (!check(AnyKeyPress))` (`display.cpp:322`); called from the serial task that
would hold the CLI with no BLE dismissal available — ISSUE-6 and ISSUE-19's blocking
problem, self-inflicted. All three existing `radioHasMemForWifi()` sites pass `true`;
this one deliberately does not.

**On success the same helper emits an `info` frame carrying the post-`begin()` largest
DMA block.** This gates nothing. ISSUE-12 records, as **SUSPECTED**, that `dma largest`
at `webui post-begin` predicts the outcome better than free heap does — 6,900 and 6,644
served, 6,132 did not — on one set of three runs with no threshold bisected. Reporting
the number turns that correlation into data the app and every future run accumulate,
without promoting it to a gate it has not earned.

### `radio_mem.h`'s three Serial lines

`:48`, `:62` and `:67` become `log_e`, making the WiFi-teardown decision in
`radioHasMemForBle()` observable for the first time. This is the smallest change in the
set and the one that unblocks the most: ISSUE-19's attribution cannot be settled by any
future repro while those lines are addressed to a port nobody is reading.

---

## Honest limits

**ISSUE-12's marginal class is not fixed.** The 2026-07-30 run 2 failure
(`dma=18,420`) logged **no** AsyncTCP error — the entry says so explicitly: "Neither of
the first two logged an AsyncTCP error, so both looked like the 'working' profile at the
RAMLOG level." Per the follow-up measurement, TCP was accepted and HTTP **body**
allocation failed. Gate D will pass that run, correctly: the server genuinely did start.

So RC3 fixes **"`webui` claims success when the HTTP server never started"**. It does not
fix **"the HTTP server started and cannot serve a request"**. Those are different
defects, the second is downstream of everything this change touches, and covering it
would require the invented threshold rejected above. RC3 makes the marginal state
*visible* — the post-`begin()` DMA figure is now reported on success — and leaves it
ungated.

**Gate D may be unreachable in normal operation.** Gates A and C stand in front of it, so
once they are in place the conditions that produced `begin(): failed to start task` may
never reach it. This is ISSUE-28's trap: its `beginAP()` failure branch is correct, and
still **UNVERIFIED** months on, because nothing can reach it through the CLI. The
`-selftest` hook below exists to avoid repeating that.

**Still out of scope, deliberately:**

- `startMdnsResponder()`'s Serial-only failure at `:399`. mDNS failing is non-fatal —
  `mdnsRunning` already tracks it — but it costs 5.6–5.9 KB and currently fails
  invisibly. One-line follow-up.
- `displayError`/`displayWarning`'s invisible console halves (`display.cpp:317,327`).
  Two lines, but an upstream file on a hot path with dozens of callers.
- `attack_commands.cpp:174` still discards `_setupAP()`'s return.
- The device-side DHCP server that does not answer (ISSUE-12's association-vs-DHCP
  finding). Untouched.

---

## Blast radius

`_setupAP()` has three callers:

- `wifi_common.cpp:184`, via `wifiConnectMenu(WIFI_AP)` — the intended path.
- `wifi_commands.cpp:37`, the `wifi on` fallback (`return _setupAP();`). Its result
  becomes truthful; today it claims success unconditionally.
- `attack_commands.cpp:174`, which discards the return. Unchanged, noted above.

`wifiConnectMenu` has 17 call sites and **already returns `bool`**; most already check
it. Gate A changes its behaviour only in the low-DMA case, which is the point.
`WifiMenu.cpp:53` ignores the return, but that is the on-device menu, where the gate's
own `displayError` reaches the operator.

Rollback is a single revert. `radioHasMemForWifi()` also retains its documented escape
hatch — the commented `// return true;` at `radio_mem.h:35`.

---

## Testing

### Host — `test/test_webui_gate/`, mirroring `test_crash_report/`

`pio test -e native`. Covers every `WebUiStartResult` slug, the DMA figures rendered into
the line, `webUiListening()` across `CLOSED`/`LISTEN`/`ESTABLISHED`, the boundary of
`webUiDmaSufficient()`, and the single-line field-per-token shape. This is the whole
testable surface — the gates themselves query the heap and touch WiFi, so they cannot run
under `[env:native]` (`test_build_src = no`).

### Hardware, in order

The device must be reflashed so tree and ELF stay aligned, or `crashlog` reports
`match=NO` for anything that happens afterwards.

1. **Baseline.** Fresh boot → `webui -bg`. Expect success, an `info` frame carrying the
   post-`begin()` DMA figure, `state() == LISTEN`, and a station still associating and
   reaching port 80. Confirms the change did not break the working path.
2. **Gate C induction.** Fresh boot → `js` (ISSUE-17, ~18 KB) → `webui -bg`. Expect a
   **refusal** naming the real DMA block, `[CLI] Result: FALSE`, an `err` frame on both
   transports — and then the two checks that matter: **`free` back at its idle plateau**
   and **no AP in an `nmcli` scan**. Those are the only evidence the unwind actually
   released the AP; a refusal that leaks 63 KB is ISSUE-39 again.
3. **Gate D.** `webui -selftest` forces the post-`begin()` failure branch. Same
   expectations as (2), plus confirming `stopWebUi()`'s unwind ran without a panic —
   this is the path that frees a live `AsyncWebServer`, historically the most dangerous
   thing in this file (see `ws_events.cpp:6-14`).

   Mechanically: a fourth flag arg on the verb (`webuiCmd.addFlagArg("selftest")`,
   alongside `noAp`/`off`/`bg` at `wifi_commands.cpp:130-133`) is threaded into
   `startWebUi` as a parameter, and gate D substitutes a synthetic non-`LISTEN` state
   for `server->state()` when it is set. The server is genuinely started and then
   genuinely unwound — the point is to exercise the unwind and the reporting for real,
   so nothing about `begin()` itself is stubbed.
4. **Close out.** Re-read `sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9`
   against `crashlog`'s `running_elf=`, and confirm the coredump partition is clean.

`webui -selftest` ships permanently, following `crashlog -selftest`'s precedent from RC4.
A temporary forcing `#define` was rejected: the build is reproducible **per source**, so
removing the flag changes the ELF hash and the evidence would attach to a build that was
never shipped.

Per this repo's bar, results get reported as "no failure observed in N s", never "safe".

---

## Documentation this obliges

- **`docs/KNOWN_ISSUES.md`** — ISSUE-12: root cause, the fix, and the marginal class that
  remains. ISSUE-19: retire the missing-log-line evidence as invalid-by-construction and
  note the sink now makes a repro decisive. ISSUE-28: cross-reference `_setupAP()` as a
  third site of the same shape. A new entry, or a fold-in, for `_setupAP()`'s discarded
  return.
- **`docs/bruce-companion-api.md`** — correct `:347`, document the new `err` frames and
  `webui`'s now-meaningful result, bump the contract to **2.5**. The `serialcmds.cpp`
  citations at `:346` have drifted (`45,52,64,68` → `65,72,85,89`) and can be fixed in
  passing.
- **`BRUCELINK.md`** — add the `Serial`-is-native-USB-CDC fact to §Known gotchas. It is
  currently only implicit in `ram_profile.cpp`'s comment and ISSUE-22, and it is the
  reason several diagnostics in this codebase reach nobody.
- **`docs/TEST_STATUS.md`** — the `webui` row.
- The new commit's ELF hash, in `BRUCELINK.md` §Testing's table.
