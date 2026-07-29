# Headless Evil Portal — design

**Date:** 2026-07-29 · **Status:** approved, not yet implemented
**Motivates:** KNOWN_ISSUES ISSUE-19 (no remote rescue), ISSUE-1 (evilportal case), and the
"Stranding risk" row against Evil Portal in `docs/TEST_STATUS.md`.

Every code citation below was read at the line given on 2026-07-29. Line numbers drift;
grep the symbol if one does not match.

---

## Why

Evil Portal **works**. Verified end to end on ELF `5186685c0fdf19c2`, 2026-07-29: it serves
the page, answers Android `/generate_204` and iOS `/hotspot-detect.html`, accepts a
credential submission and returns it at `/creds`. ISSUE-21 turned out to be the memory
ceiling, not a routing defect.

What makes it unshippable is not the attack, it is the lifecycle. Two consequences of the
current implementation:

1. **It holds the serial task for its entire life.** `evilportalCmdCallback`
   (`attack_commands.cpp:52`) constructs a stack temporary, and the constructor calls
   `loop()` (`evil_portal.cpp:31`), which is a `while (true)` (`:297`). `serialCli.parse()`
   is called inline from `handleSerialCommands()`, so while the portal runs **no BLE
   command is parsed at all** — the bytes arrive and sit unread.
2. **It destroys the WebUI on entry.** The constructor calls
   `cleanlyStopWebUiForWiFiFeature()` (`evil_portal.cpp:29`).

Together, with `ble api off`, that leaves **no remote control surface whatsoever** — BLE
down, WebUI destroyed, and no serial CLI exists on this board (ISSUE-22). The only exit is
the on-device Esc chord → "Exit Portal". An app must therefore never dispatch it without
an operator at the board.

This design removes reason 1 and adds an unconditional time cap, which together retire the
stranding risk and make the ISSUE-1 evilportal case retestable under real load.

## Scope

**In:** a background lifecycle for `EvilPortal`, a pump, start/stop/status verbs, a hard
duration cap, a bench script.

**Out:** the memory ceiling. Whether a portal and an armed BLE stack coexist well enough to
serve pages is **UNVERIFIED** and this design does not attempt to fix it — it measures it
and reports it. Also out: fixing `~EvilPortal()` (see "Deliberately not doing").

## Architecture

### The trick

`EvilPortal`'s constructor already branches on `backgroundMode`:

```cpp
    if (!setup()) return;
    cleanlyStopWebUiForWiFiFeature();
    beginAP();
    if (!_backgroundMode) { loop(); }        // evil_portal.cpp:28-31
```

With `backgroundMode = true` the constructor runs `setup()`, stops the WebUI, brings up the
AP, and **returns immediately**. The serial task is never held. That is the whole mechanism;
karma already relies on it (`karma_attack.cpp:1786`).

### What actually needs pumping

`processRequests()` (`evil_portal.cpp:374-384`) does exactly two things: clear a pending
WiFi restart, and `dnsServer->processNextRequest()`.

**The AsyncWebServer is self-driving** — it runs on its own task. So the pump serves the
captive-portal DNS redirect (the automatic popup), not page serving. A starved pump degrades
discovery; it does not stop `192.168.4.1` from answering a browser pointed at it. This
lowers the stakes on pump placement considerably.

### Components

**New file `src/modules/wifi/evil_portal_bg.{h,cpp}`** — fork-owned, so the merge surface
against upstream stays small (BRUCELINK "Upstream files" convention).

```cpp
bool   evilPortalBgStart(const String &ssid, uint8_t channel,
                         const String &templateFile, uint32_t maxSeconds);
bool   evilPortalBgStop();
bool   evilPortalBgRunning();
void   evilPortalBgTick();
String evilPortalBgStatus();
```

File-static state: `EvilPortal *portal`, `startedMs`, `maxMs`, `lastCredCount`.

**Pump site: the serial command task.** `evilPortalBgTick()` is called from
`handleSerialCommands()` (`serialcmds.cpp`), which the task loop already runs every 10 ms
(`serialcmds.cpp:88-92`).

It must be called **at the very top of the function**, before the `hasLine()` early return
at `serialcmds.cpp:71`. Placed after it, the pump would only run when a command happened to
be waiting — i.e. almost never.

Rejected alternatives:
- *Dedicated FreeRTOS task* — most robust, but its stack comes out of internal DRAM, the
  exact resource BRUCELINK says the RAM sampler was disabled over. Not affordable at the
  ~15 KB margin that applies when BLE is armed.
- *Main loop* — where upstream karma effectively sits (`checkPortals()` at
  `karma_attack.cpp:2731`). No new stack, but the main loop sinks into `loopOptions()`
  whenever an on-device menu opens, so an operator touching the device starves the pump.

The chosen site starves only while a blocking verb holds the serial task, and dispatching a
blocking verb during a headless portal is already an operator error.

### Changes to `evil_portal.cpp` (upstream file — three, all small)

1. **Extract `void EvilPortal::shutdown()`** from `loop()`'s exit path
   (`evil_portal.cpp:350-360`): `webServer.end()`, `dnsServer->stop()`,
   `WiFi.mode(_originalWifiMode)`, `wifiDisconnect()`, preserving the existing
   `vTaskDelay` spacing between them. `loop()` keeps its own
   `displayTextLine("Shutting down...")` (`:347`) and then calls `shutdown()`.
   The headless path calls `shutdown()` **without drawing anything** — drawing from the
   serial task is the ISSUE-1 trigger and must not reappear here.
2. **Add `bool isReady()`.** The constructor's `if (!setup()) return;` (`:28`) fails
   **silently with no outward signal** — `beginAP()` never runs and the caller cannot tell.
   A flag set after `beginAP()` gives the background starter a way to detect a dead object
   instead of reporting a portal that does not exist.
3. **Add `int getCredentialCount()`.** `hasCredentials()` (`:386`) returns
   `totalCapturedCredentials > 0`, a bool that can only transition once. Detecting the
   *second* capture needs the count.

### Deliberately not doing

`~EvilPortal()` is empty (`evil_portal.cpp:34`). Karma's `destroyActivePortal()`
(`karma_attack.cpp:684-696`) therefore deletes the instance while **never stopping the
shared DNS server and never restoring `_originalWifiMode`** — it relies on the `webServer`
member destructor alone.

That is a real defect, but fixing the destructor silently changes karma's behaviour, and
karma is upstream code with no test coverage here. It is recorded as a separate finding
rather than smuggled into this change.

## Verb surface

```
evilportal [ssid] [channel] [template] -bg [-duration <sec>]
evilportal -off
evilportal -status
```

Bare `evilportal` keeps today's blocking, on-device behaviour untouched. Flags are checked
**before** the positional start path, so `-off` and `-status` can never start a portal.

This mirrors `webui`, which already uses `-noAp` / `-off` / `-bg`
(`wifi_commands.cpp:125-127`), so the app and the operator meet one idiom rather than two.

**Parsing is VERIFIED against the library source, not assumed.** No other command in this
codebase mixes positional and flag arguments, so it was checked directly:

- `cmd.c:271-276` — a `-`-prefixed word matches an unset arg **by name**; a bare word
  matches the first unset `ARG_POS`.
- `Command.cpp:104-110` — `addPosArg(name, default)` calls `arg_create_opt_positional`,
  i.e. **optional**, so `req == false`.
- `cmd.c:310-316` — the final check errors only on `a->req && !a->set`.

Therefore `evilportal -off` binds the `off` flag and leaves ssid/channel/template unset at
their defaults, and parsing succeeds.

**Known parsing limitation:** an SSID beginning with `-` is read as a flag name and returns
"unknown argument". Document it; do not work around it.

## Lifetime and the duration cap

`evilPortalBgTick()` enforces a hard wall-clock cap: `millis() - startedMs >= maxMs` → stop.

- `-duration` defaults to **600 s**.
- `-duration 0` means unlimited, for when the operator is physically at the board.

This is intentionally **not** the existing `checkAndExtendDuration()` machinery, which
extends the deadline on activity (`evil_portal.cpp:419`, called only by karma at
`karma_attack.cpp:1709`). A portal that is actually being used would never expire, which is
exactly backwards for a measure whose purpose is to guarantee recovery.

`shouldTerminate()` (`evil_portal.h:50`) has **zero callers anywhere in the tree** — it is
dead code. Two consequences: the cap is new machinery rather than reuse, and it does
**not** explain the unexplained portal self-exit recorded in engram obs #1782. That
remains open and unattributed.

## Data flow and app feedback

`setDeviceState("portal")` / `setDeviceState("idle")` already emit `state` frames
(`attack_commands.cpp:51,53`), so portal start and stop are already visible to the app. The
background path keeps this.

Credentials reuse the existing **`log`** frame: `evilPortalBgTick()` compares
`getCredentialCount()` against `lastCredCount` and pushes a log event on an increase.

No new event frame type is introduced. BRUCELINK records that exactly four frames are
emitted — `state`, `log`, `ble_progress`, `ble_result` — and a fifth would make the
companion app's vendored contract copy (`maritest/vendor/`) stale.

`evilportal -status` prints, on demand over BLE: running yes/no, SSID, channel, uptime,
credential count, seconds remaining on the cap.

## Error handling

Every one of these currently fails silently. This repo has been bitten repeatedly by that
(ISSUE-7 "Result: TRUE is hardcoded", the "silence is ambiguous" rule in TEST_STATUS.md), so
each gets a distinct printed reply:

| Condition | Reply |
|---|---|
| start while one is running | refuse, name the running SSID |
| `new (std::nothrow)` returns null | "out of memory", plus free heap and largest DMA block |
| constructor's `setup()` failed (`isReady()` false) | delete the object, report the failure — this is the case that would otherwise leave a zombie reporting success |
| `-off` with nothing running | say so; do not report a successful stop |
| start succeeded | include free heap and largest contiguous DMA block |

Free heap is reported on start because whether an armed BLE stack and the portal AP coexist
is the open question this design cannot answer by reasoning.

## Testing

**Host-side (`pio test -e native`) gains nothing here.** Every path touches Arduino,
FreeRTOS, WiFi or AsyncWebServer, so none of it builds under `[env:native]`
(`test_build_src = no`). Stating this explicitly rather than leaving a silent gap.

**Bench: `tools/ble_spike/portal_bg.py`.** Discovers by service UUID
`4371ec0b-3d43-49f9-b731-7c72a4a7bb91`, never by name.

1. BLE armed → `evilportal PortalTest 6 -bg -duration 300`; record heap and DMA block from
   the reply.
2. **Immediately send `uptime` over BLE.** This is the load-bearing assertion of the whole
   design: today the serial task is held for the portal's entire life, so a reply here
   proves the portal is genuinely headless. If this fails, nothing else matters.
3. Poll `evilportal -status`.
4. Laptop joins the AP → `GET /`, submit credentials, confirm a `log` frame arrives on the
   event characteristic.
5. `evilportal -off` over BLE → AP gone, BLE still alive, `state` back to idle.
6. Separately, unattended: let `-duration` expire and confirm self-teardown.
7. `usbwatch2.py` capturing `/dev/ttyACM0` throughout — the only place a panic backtrace
   appears. Run exactly one instance.

**ISSUE-1 retest.** The portal no longer draws from the serial task, so the evilportal
under-load case becomes testable against the `2d9422ea` mutex fix for the first time. An
idle portal proves nothing — load must be applied.

### What is expected to be uncertain

Step 4 may starve. Obs #1784 proved the portal serves pages with `ble api off` (121 KB
free); it has **never** been shown to serve them with BLE armed, and ISSUE-21's original
symptom was precisely this starvation. Recording the prediction in advance so the result
counts either way.

If step 4 fails, the design still delivers: the stop channel, the duration cap, and the
ISSUE-1 retest all hold. The harvest itself would remain a BLE-off operation, with the cap
as the sole recovery path — which is still strictly better than today's "physical access
only".

## Success criteria

1. BLE answers a command while a headless portal is running. **This is the definition of
   done**; without it the change has achieved nothing.
2. `evilportal -off` over BLE stops the portal and restores WiFi state.
3. The duration cap fires unattended and tears the portal down.
4. Bare `evilportal` behaves exactly as before.
5. Every failure path prints something distinguishable from success.
6. Heap and DMA figures with a headless portal + BLE armed are recorded, whatever they say.
