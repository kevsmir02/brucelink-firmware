# Bruce Companion App — API Contract

**Source of truth:** this file, audited against the firmware tree. Where any
earlier design note disagrees with what is written here, the code wins. See
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) for *why* the firmware was changed,
and [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) for verified defects — read that before
planning against any verb; this document covers *what the interface is*.

**Contract version:** 2.3 — audited against `0b2073fa`, re-verified on hardware
2026-07-29 against `c9c43c03` (which fixed the `fastpair_*` no-op and the lost
name/UUID/BT-MAC after a spam, §5.1), then extended by a verb-surface and HTTP sweep
the same day. New in 2.3: the AP gateway is `172.0.0.1` not `192.168.4.1` (§4); HTTP
auth verified and `POST /login` found to abort the device under load (ISSUE-18); the
JS interpreter has no return channel (ISSUE-15) and retains ~18 KB (ISSUE-17);
`encrypt`/`decrypt` round-trips fail ~62% silently (ISSUE-13); `settings` writes are
silent no-ops for most fields (ISSUE-14). Fork point from upstream Bruce: `59e83bfb`
(2026-07-23); companion work starts at `373fb5d8` (2026-07-25). Bump this line
whenever the contract changes.

> **Read `docs/KNOWN_ISSUES.md` §ISSUE-12 before planning around HTTP.** The WebUI
> works from a fresh boot but starts with roughly 18 KB of margin, and silently fails
> to start at all if anything consumed heap first. BLE control and HTTP do coexist —
> `systeminfo` answered over BLE with the AP up and `free_heap:14140` — but the
> combination has no headroom.

**Line numbers drift.** Every citation below was re-checked at `0b2073fa` and points
at real code, but a citation is a pointer, not a guarantee — grep for the symbol if
the line does not match.

**How to read this doc.** Every claim below is either code (file:line given) or a
measurement (device + date given). Anything not yet verified on hardware is marked
**UNVERIFIED**. Nothing here is aspirational — planned-but-unbuilt items live in
§8.

---

## 1. Transports

Two control transports, both speaking **the same CLI command bus** (`SerialCli::parse`).
Every verb works identically over either.

| | BLE GATT serial | WiFi HTTP |
|---|---|---|
| Role | **Primary** control + events | Bulk transfer (screenshots, files) |
| Survives a WiFi attack | yes | no — the attack tears the server down |
| Survives a BLE attack | no — suspended for the duration | yes |
| Request/response | CLI char, EOT-terminated | `POST /cm` (fire-and-forget, no output) |
| Events | event char (`d555ed98-…`) | `/ws` WebSocket |

**Why BLE is primary** (`src/main.cpp:491`): every WiFi attack destroys the medium
an HTTP connection depends on. A BLE connection survives both `deauth` and Evil
Portal and carries CLI traffic throughout — verified on smoochiee-board 2026-07-27.

**The two transports are not meant to run simultaneously.** Fully loaded with BLE
API + WiFi AP + WebUI the board has ~15 KB free (measured **14,951 bytes**,
2026-07-29), and a single associated station drives that to a few hundred bytes, at
which point notifies truncate and HTTP dies
(`src/core/serial_commands/wifi_commands.cpp:58`). Use `webui -off` to hand the
WiFi memory back when done with bulk transfer, `webui -bg` to bring it back. Both
verified over BLE 2026-07-29; see §7.1 for the full heap figures.

---

## 2. BLE GATT serial (primary)

| Item | Value | Source |
|---|---|---|
| Advertised name | `Bruc` | `ble_api.cpp:67` |
| Service UUID | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` | `BLESerialService.cpp:66` |
| CLI characteristic | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` — READ \| NOTIFY \| WRITE | `BLESerialService.cpp:70` |
| Event characteristic | `d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9` — READ \| NOTIFY | `BLESerialService.cpp:80` |
| Battery service | `0x180F` / `0x2A19` (READ \| NOTIFY) | `BatteryService.cpp` |
| Response terminator | `0x04` (EOT) | `BLESerialService.h:22` |

### 2.1 Framing

**Commands** are newline-terminated writes to the CLI characteristic. The firmware
buffers RX in a 512-byte ring and only parses on a complete line
(`serialcmds.cpp:59` uses `hasLine('\n')`, not `available()`), so a command split
across several writes reassembles correctly.

**Responses** stream back as notifies on the CLI characteristic, then a human
prompt `"# "`, then a single `0x04` EOT byte. Read until EOT. Do **not** use the
`"# "` prompt as a terminator — any output line beginning with `# ` (a dumped
markdown file, a commented config) would truncate the response and desynchronise
every command after it.

**Chunking:** notifies are split to `negotiated_MTU - 3` bytes
(`BLESerialService.cpp:150`). **The app must call `requestMTU(247)` on connect** —
Android's default 23 gives 20-byte chunks. Measured 2026-07-27: BlueZ negotiates
128 (125-byte chunks); nRF Connect on Android never requests more and stays at 23.

**Truncation is reported in-band.** If a chunk exhausts its notify retries (device
out of memory), the reply is aborted and `endOfResponse()` prepends
`\r\n[TRUNCATED: device low on memory]\r\n` before the EOT
(`BLESerialService.cpp:178`). Treat this as a distinct error from a parse failure.

**No request IDs.** The CLI carries none, so responses correlate **positionally**.
Do not pipeline commands unless you are tracking order yourself.

### 2.2 Events

Event frames go out on their own notify-only characteristic so async JSON never
interleaves with CLI stdout. Frames are `\n`-delimited; the stream is chunked the
same way, so split on newline after reassembling.

Events are **dropped, not queued**, when nobody is subscribed
(`BLESerialService.cpp:196`) and the subscription is cleared on peer disconnect
(`ble_api.cpp:79`). Resume via `lastEventId` — see §4.3.

---

## 3. WiFi HTTP (bulk transfer)

- Bruce AP or shared LAN. mDNS `http://<host>.local/`.
- Auth: cookie `BRUCESESSION=<token>` after `POST /login`, **or**
  `Authorization: Bearer <token>` (`webInterface.cpp:176` — checked before the
  cookie; `/login` also returns `{token}` as JSON).

| Method | Path | Purpose |
|---|---|---|
| POST | `/cm?cmnd=<verb>` | Queue any CLI verb. **POST only** (`webInterface.cpp:532`) — GET 404s |
| GET | `/getscreen` | Binary TFT draw-log pull |
| GET | `/systeminfo` | JSON system info (§3.1) |
| GET | `/listfiles?folder=&fs=` | Directory listing |
| GET | `/file?name=&action=&fs=` | download/image/delete/create/createfile/edit |
| POST | `/edit` | write file |
| POST | `/upload` | upload file |
| POST | `/rename` | rename file/folder |
| POST | `/login` `{user,pwd}` | → `Set-Cookie BRUCESESSION=`, plus `{token}` |
| GET | `/logout` | invalidate session |
| GET | `/reboot` | `ESP.restart()` |
| WS | `/ws` | Event stream (§4) |

`/cm` returns `200 "command <verb> queued"` on accept, `400 "command failed…"` when
the queue is full. **It never returns command output** — output goes to `/ws` as
`log` frames. Use the BLE CLI characteristic when you need the actual reply text.

`/cm cmnd=nav <sel|esc|up|down|next|prev> [ms]` is special-cased: it synthesises a
button press and returns immediately without queueing.

### 3.1 `/systeminfo`

Built by `buildSystemInfoJson()` (`src/core/system_info.cpp`), also reachable as
the `systeminfo` CLI verb over BLE.

```json
{
  "BRUCE_VERSION": "dev",
  "SD":       {"free":"…","used":"…","total":"…"},
  "LittleFS": {"free":"…","used":"…","total":"…"},
  "capabilities": {
    "usb_as_hid": true, "has_screen": true, "lite_version": false,
    "has_cc1101": false, "has_nrf24": false, "has_pn532": false,
    "has_gps": false, "has_ir": false, "has_fm": false, "has_eth": false,
    "has_buzz": true, "has_rgb_led": true, "has_mic": true
  },
  "battery_pct": 73, "charging": false,
  "wifi_mode": 2, "ip": "0.0.0.0",
  "free_heap": 142336, "psram": true
}
```

Field notes — these bit us, read them:

- **`wifi_mode` is an integer**, the raw `WiFi.getMode()` enum
  (`system_info.cpp:78`): `0`=OFF, `1`=STA, `2`=AP, `3`=APSTA. It is **not** the
  string `"STA"`.
- **`ip` is `WiFi.localIP()`** (`system_info.cpp:79`), which reads `0.0.0.0` in AP
  mode. **The WebUI AP gateway is `172.0.0.1`, not `192.168.4.1`** — measured
  2026-07-29: a laptop joining `BruceNet` got `172.0.0.3/24`, default via `172.0.0.1`,
  and `GET http://172.0.0.1/` returned 200. `192.168.4.1` is the *Evil Portal*
  gateway override (§5.2), a different code path; this document previously conflated
  the two.
- `has_pn532`, `has_fm`, `has_eth` are **hardcoded `false`**
  (`system_info.cpp:47,58,59`) regardless of the hardware. Do not trust them.
- **No capability flag is a runtime probe. Do not gate app UI on `capabilities`.**
  The others are compile-time `#if defined(...)` checks against the *board profile*,
  not against what is fitted. On the reference unit — a bare ESP32‑S3‑N16R8 with an
  IPS LCD, five buttons and nothing else — `/systeminfo` reports `has_cc1101`,
  `has_nrf24`, `has_gps`, `has_ir`, `has_buzz`, `has_rgb_led` and `has_mic` all
  `true`, while `i2c` on the same device returns `No I2C devices found`
  (measured 2026-07-29). `boards/smoochiee-board/pins_arduino.h` describes a fully
  populated Smoochiee V2; the flags describe that profile, not your board. Keep an
  app-side truth table instead.
- **`battery_pct` and `charging` are only meaningful if a PMU is fitted.**
  `getBattery()` and `isCharging()` call `PPM.*` unconditionally and ignore the
  `pmu_ret` result of `PPM.init()` (`boards/smoochiee-board/interface.cpp:38-75`).
  With no BQ25896 on the bus the reads fail, voltage comes back 0, the percentage
  computes negative and `if (percent < 0) return 1` clamps it. Measured on the
  reference unit 2026-07-29: **`battery_pct` is permanently `1` and `charging`
  permanently `true`**, and the failing polls emit a continuous
  `i2cWrite(): ... ESP_ERR_INVALID_STATE` stream on the console (~10 per 22 s).
  The example above is from a board that has the PMU.

---

## 4. Event frames

Same JSON on both transports (`/ws` and the BLE event characteristic) — one
`pushWsEvent()` builds the frame and fans it out (`ws_events.cpp:56`).

### 4.1 Frames actually shipped

```ts
type EventFrame =
  | { id:number, type:'state',        device_state:string }
  | { id:number, type:'log',          line:string, level:'info'|'warn'|'err' }
  | { id:number, type:'ble_progress', msg:string }
  | { id:number, type:'ble_result',   success:boolean, msg:string };
```

`device_state` values emitted today: `idle`, `portal`, `ble_spam`
(`attack_commands.cpp:51,53,114,123`). No other verb sets state.

`log` frames are emitted for every CLI command dispatch and its result
(`serialcmds.cpp:45,52,64,68`) — `COMMAND: <text>` then `[CLI] Result: TRUE|FALSE`.
That is the only `log` source; general CLI stdout is **not** forwarded to `/ws`.

**Use these two frames as your dispatch ACK and completion signal.** `COMMAND:` is
pushed *before* `serialCli.parse()` runs and `[CLI] Result:` *after*
(`serialcmds.cpp:64` then `:68`), so the ACK reaches the app even for a verb that
then blocks for minutes. Measured over BLE 2026-07-29 — the two frames land 2 ms
apart for a fast verb, and an unknown verb yields `[CLI] Result: FALSE`:

```
+ 297ms  {"id":61,"type":"log","line":"COMMAND: uptime","level":"info"}
+ 299ms  {"id":62,"type":"log","line":"[CLI] Result: TRUE","level":"info"}
+1618ms  {"id":65,"type":"log","line":"COMMAND: nosuchverb","level":"info"}
+1621ms  {"id":66,"type":"log","line":"[CLI] Result: FALSE","level":"info"}
```

This matters because a blocking verb emits **no CLI reply, prompt or EOT until it
exits** (§5.2), so any app-side exec timeout shorter than the run will fire on a
perfectly healthy command. Do not infer failure from that timeout. Ack on
`COMMAND:`, complete on `[CLI] Result:`.

> ⚠️ **`[CLI] Result:` is a completion signal, not a success signal.** Every attack
> verb's callback ends in a bare `return true` and discards the outcome of the work
> it dispatched (`attack_commands.cpp:147-175`), so `TRUE` means "the callback
> returned", never "the attack worked". Measured 2026-07-29: `reverseshell` reported
> `[CLI] Result: TRUE` 30 ms after its AP creation had failed outright
> (`[E][AP.cpp:225] create(): passphrase too short!` on the console). Rendering that
> as success would show a green tick for an attack that never started. There is no
> in-band failure signal for these verbs today — see
> [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) §ISSUE-7. `blespam` is the exception: it
> validates its arguments and emits real `ble_result` telemetry.

`ble_progress` / `ble_result` come from `showAttackProgress` / `showAttackResult`
(`BLE_Suite.cpp:5994,6047`) and the spam loop (`ble_spam.cpp:1558,1578,1585`).

**Not implemented:** `cred`, `host`, `ap`, `packet`. The v1.x contract listed them
in the frame union; no code emits them. Evil Portal creds must be pulled per §6.

### 4.2 WebSocket connect

On connect the server pushes one frame with the **current** event ID and state
(`ws_events.cpp:27`):

```json
{"id":<current wsEventId>,"type":"state","device_state":"idle"}
```

The id is `0` only on a fresh boot, not on every connect.

**`{cmd:"subscribe", since:…}` is ignored.** The v1.x contract specified it; the
handler is a no-op (`ws_events.cpp:29`). There is no server-side replay — the app
re-fetches `/systeminfo` to resume.

### 4.3 Event IDs across a transport switch

`wsEventId` increments **unconditionally**, even when the WebSocket is gone
(`ws_events.cpp:62`). A WiFi attack tearing the WebUI down is exactly the case BLE
covers, so the BLE stream carries a gap-free continuation of the same ID space.
An app tracking `lastEventId` can detect what it missed across a switch — but it
cannot ask for a replay (§4.2); it can only detect the gap.

---

## 5. Verbs

Registered by `createAttackCommands()` (`src/core/serial_commands/attack_commands.cpp:177`)
unless noted.

**Two separate properties, previously conflated under one "Blocks serial?" column.**
*Every* verb occupies the serial task while it runs — `handleSerialCommands()` calls
`serialCli.parse()` inline (`serialcmds.cpp:66`) — so the question is never *whether*
a verb blocks but *for how long*, and whether it can end without a human. An app
needs both answers, so they are now separate columns.

| Verb | Needs a button on the device? | CLI reply arrives | Telemetry | Notes |
|---|---|---|---|---|
| `ble api on\|off` | no | immediately | — | toggles the GATT server; persists across reboot (§5.4) |
| `systeminfo` | no | immediately | — | same JSON as `GET /systeminfo`; the way to read it over BLE |
| `free` | no | immediately | — | one-line heap report incl. **largest contiguous DMA block**, which is what actually gates radio init |
| `webui -off` | no | immediately | — | stops the WebUI **and its AP**; frees the memory for BLE. Verified 2026-07-29 |
| `webui -bg` | no | immediately | — | starts the WebUI and returns instead of holding the screen. Returned in 357 ms, 2026-07-29. Still prints "Press ESC to quit" — stale text, it does return |
| `blespam <type> <count>` | no | **after the burst** | `state`, `ble_progress`, `ble_result` — but see below | self-completing, verified 5/5. `fastpair_*` fixed in `c9c43c03` (§5.1). Suspends the BLE link for **0.5–11.9 s** (measured); tolerate ~12 s. Types in §5.1; count < 1 → 10 |
| `evilportal <ssid> <ch> [template]` | **YES** | **only after dismissal** | ✅ `state: portal` | defaults: ssid `Free Wifi`, ch `6` (out-of-range → 6); gateway forced to `192.168.4.1`. Tested 2026-07-29: no crash in 100 s **idle**; `state` frame at +3.51 s. The only attack verb with usable telemetry. See §5.3 — no timeout, no remote stop |
| `deauth [<target>]` | ☠️ **CRASHES THE DEVICE** | never — it panics | — | see below and KNOWN_ISSUES §ISSUE-1. `target` is parsed but ignored (`attack_commands.cpp:152`) |
| `karma` | **YES** | only after dismissal | — | opens the TFT menu. Tested 2026-07-29: blocks, no crash in 90 s |
| `blesniffer` | **YES** | only after dismissal | — | opens the BLE Suite menu. Tested: blocks, no crash in 90 s; the BLE control link survives |
| `ap_info` | **YES** | only after dismissal | — | Tested: blocks, no crash. **Exits on SELECT only** — Esc is ignored (§ISSUE-6) |
| `pwngrid` | **YES** | only after dismissal | — | starts Brucegotchi. Tested: blocks, no crash in 90 s |
| `reverseshell` | no | **immediately** | — | **does not block** — returned in 90 ms. But it returned `Result: TRUE` while its AP creation had failed (§ISSUE-7) |

> ☠️ **`deauth` panics the device, reproducibly.** Not "blocks" — panics.
> `assert failed: xTaskPriorityDisinherit` inside `spiEndTransaction`, ~20–70 s after
> dispatch, no further user action needed. The menu-dispatcher verbs draw to the TFT
> from the **serialcmds task** while the main loop task draws the same display over
> the same SPI bus, and the bus mutex ends up released by a task that does not own it.
> Full decoded backtrace and root cause in
> [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) §ISSUE-1. Verified 2/2 on 2026-07-29.
>
> **`deauth` is the only verb that crashes.** All five other menu-dispatcher verbs
> were individually tested on 2026-07-29 with the console captured: `karma`,
> `blesniffer`, `ap_info` and `pwngrid` block but survive; `reverseshell` does not
> even block. The exception is instructive — `ap_info` draws through
> `ScrollableTextArea`, which redraws only on input, while `deauth`'s `loopOptions`
> path drives `drawArc` continuously. The collision needs *sustained* drawing from
> the serial task, so a quiet UI is not the same as a safe one, and a clean 90 s
> window is not proof (`deauth` itself took 70–130 s to fall over on its first run).
>
> **App impact:** the crash reboots the device, `bleApiAutoStart` re-arms the BLE API,
> and the app sees a *reconnect* rather than an error. That looks like a transport
> glitch, so an app that retries the verb on reconnect will crash-loop the hardware.

`webui`, `systeminfo` and `free` live in `wifi_commands.cpp` / `util_commands.cpp`,
not `attack_commands.cpp`.

### 5.1 `blespam` types

Two engines behind one verb (`attack_commands.cpp:74`):

- **FastPair popup engine:** `fastpair_regular`, `fastpair_fun`, `fastpair_prank`,
  `fastpair_custom` — **fixed in `c9c43c03`**; transmits valid Fast Pair adverts.
  It previously built the payload as raw AD structures and handed them to
  `setManufacturerData()`, which wrapped them again, so the advert went out under
  company ID 0x0303 and no scanner saw it — 60 adverts, zero popups on 2026-07-29.
  Now packet-captured at 8–16 valid `0xFE2C` adverts per run, and **confirmed at
  handset level 2026-07-29** — an Android showed the Fast Pair popup during
  `blespam fastpair_regular 900`. The earlier Android null result was the handset's
  "Scan for nearby devices" being off, not a firmware fault. **iPhones do not
  implement Fast Pair** (`0xFE2C` is a Google protocol) — use `apple` for iOS. See
  [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) §ISSUE-8.
- **Generic spam engine** (`bleSpamAttackTypeFromName`, `ble_spam.cpp:1592`):
  `apple`, `android`, `ibeacon`, `samsung`, `windows` (alias `swiftpair`),
  `random` (alias `all`). All types were packet-captured on 2026-07-29 and
  re-verified after `c9c43c03`:

  | Type | Transmits correctly | Still discoverable as `Bruc` after? |
  |---|---|---|
  | `apple` | ✅ company 76; iPhone showed "Setup New iPhone" | ✅ |
  | `android` | ✅ `0xFE2C` Fast Pair service data | ✅ |
  | `ibeacon` | name-only advert, **not** an Apple iBeacon | ✅ (was ⚠️) |
  | `samsung` | ✅ company 117, but see §ISSUE-10 | ✅ (was ⚠️) |
  | `windows` | ✅ company 6, "Generic Swift Pair" | ✅ (was ⚠️) |
  | `random` | selects across all types | ✅ |

  Before `c9c43c03` the ⚠️ types left the device healthy and advertising but
  **without its name or service UUID**, because the spam's advertisement payload was
  never cleared and the 31-byte budget overflowed when the BLE API re-advertised. The
  BT MAC was not restored either. Both are fixed and all six types now retain name,
  service UUID and factory MAC. See [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) §ISSUE-9.

  **Still do not discover by name alone.** Match on the service UUID
  `4371ec0b-3d43-49f9-b731-7c72a4a7bb91`. Name discovery now survives every type
  tested, but the UUID is the cheaper invariant to rely on and it cost four bogus
  "device is bricked" conclusions to learn that once.
- **`menu`** — opens the interactive on-device UI and drives its own radio
  lifecycle (no transport swap)

There is no bare `fastpair` and no `ninebot`, despite the v1.x contract listing
both. Unknown types are rejected **before** any radio teardown, so a typo does not
flap the AP.

### 5.2 Blocking verbs

`/cm` queues onto a **depth-2** FreeRTOS queue (`serialcmds.cpp:91`) drained one at
a time by the serial task. Verbs in the **YES** column enter a `loopOptions()` on the
TFT that does not return until the user presses a physical button. While blocked:

- the queue fills → all further `/cm` calls return **HTTP 400**;
- `/ws` and the BLE event characteristic stay alive. The `COMMAND: <verb>` frame for
  the blocking verb **is** delivered, because it is pushed before `parse()` (§4.1);
  its `[CLI] Result:` frame is not, until the verb exits. No other frames are pushed
  meanwhile unless the verb itself emits them.

**BLE cannot rescue a blocking verb; HTTP can.** The serial task is what drains the
BLE characteristic (`serialcmds.cpp:59`), so while it is blocked no BLE command is
parsed — including `nav esc`. The bytes do arrive (`onWrite` runs on the NimBLE host
task and pushes into the RX ring, `BLESerialService.cpp:25-27`); nothing acts on
them. `POST /cm cmnd=nav esc` is different: it is special-cased in the AsyncWebServer
task *before* queueing and writes the button globals directly
(`webInterface.cpp:532`), and every attack loop polls `check(EscPress)`. **Code-verified,
not yet hardware-tested** — if you rely on it, test it first.

App handling: badge these verbs "Requires on-device interaction". They do **not**
emit a `state` frame, so the only reliable idle signals are the next `/cm`
succeeding, or a `free`/`systeminfo` round-trip over BLE.

A future patch could run them in a dedicated task; not done.

### 5.3 `evilportal` has no timeout and no remote stop

`evilportalCmdCallback` (`attack_commands.cpp:51`) constructs a **stack-local**
`EvilPortal(ssid, channel, false, false, /*autoMode=*/true, /*backgroundMode=*/false,
templateFile)`. Because `backgroundMode` is false the constructor calls `loop()`
(`evil_portal.cpp:31`), and `EvilPortal::loop()` (`evil_portal.cpp:290-372`) has
**exactly one exit path**: `check(EscPress)` → `loopOptions` → "Exit Portal" →
`return`. There is no duration check in that loop —
`checkAndExtendDuration()`/`_baseDurationSec` exist but are only called on the
background instance Karma owns. So the portal runs until a human presses ESC on the
device and picks "Exit Portal".

A headless path already exists and is in production use: Karma heap-allocates
`new EvilPortal(ssid, channel, false, false, true, /*backgroundMode=*/true, ...)`
(`karma_attack.cpp:1786`) and pumps it with `instance->processRequests()`
(`karma_attack.cpp:1747`), which early-returns unless `_backgroundMode`
(`evil_portal.cpp:375`). Flipping the flag in the CLI verb alone would **not** work —
the stack temporary is destroyed at the end of the statement, killing the portal.
Making `evilportal` one-tap means heap-allocating it, holding the pointer, pumping
`processRequests()` from the main loop, and adding a stop verb. Not done.

### 5.4 BLE API persistence and boot

`ble api on|off` (and the Config-menu toggle — both go through `enableBLEAPI()`)
writes `bleApiAutoStart` to the config file via `bruceConfig.setBleApiAutoStart()`
(`settings.cpp:1691`; stored and reloaded at `config.cpp:69,377,767`). At the **end**
of `setup()`, after WiFi init, the firmware re-arms the GATT server if that flag is
set (`main.cpp:588`). Default is `0` — a device that has never been told
`ble api on` boots without it.

Verified end to end on hardware 2026-07-29: a device left running 57 minutes with no
`ble api on` sent that session was advertising as `Bruc`, and `settings
bleApiAutoStart` over BLE returned `1`.

This is **not** the reverted `eb05177b` behaviour. That commit started BLE
unconditionally *before* WiFi init to win the DMA race and was reverted in
`e2631370` when it crashed. The current path is opt-in, runs last, and relies on
the two radios never being loaded at once.

`bleApiSuspend()` / `bleApiResume()` deliberately do **not** touch the persisted
flag — a swap around an attack is not the user changing their mind
(`settings.cpp:1731`).

---

## 6. Evil Portal credentials

No `cred` event frame exists (§4.1). Poll
`GET http://<portal.softAPIP>/<getCredsEndpoint>` (endpoint configurable via
`bruceConfig.evilPortalEndpoints`) and parse the returned HTML `<ol>` of
`key: value<br>` lines app-side. Fallback: CSV pull via `/file` after the mission
ends.

The portal binds port 80 with `SO_REUSEADDR` (`7de5b1b9`) so it serves pages
immediately after the WebUI has released the port.

---

## 7. Radio coexistence — measured

Everything in this section is a hardware measurement on **smoochiee-board**
(no PSRAM available to BT), 2026-07-27/28.

### 7.1 The constraint

`radioHasMemForBle()` (`core/radio_mem.h`) admits a BLE attack only when the
**largest contiguous DMA-capable block** clears `RADIO_BLE_MIN_DMA_BLOCK` (15 KB).
PSRAM cannot back BT DMA. When the check fails its fallback calls
`wifiDisconnect()` to free memory — so with the BLE API up it is **the memory
guard, not the attack**, that destroys the AP. This is a crash guard, not a bug:
bypassing it half-initialises `esp_bt_controller_init` and panics (tried and
reverted, `e2631370`).

Free heap alone does not predict this. Use `free`, which reports the largest
contiguous DMA block explicitly.

Measured 2026-07-27, fully loaded (BLE API + AP + WebUI) **with a station
associated**: largest DMA block **1,332 bytes** — the guard cannot pass. After
`bleApiSuspend()` releases ~62 KB: **32,756 bytes** — the guard passes on its first
check and never touches WiFi.

Re-measured 2026-07-29 on the same board **with no station associated**, via `free`
over BLE. The 1,332-byte figure did not reproduce in that state; the conclusion is
unchanged, since every loaded figure is still far below the 15 KB guard:

| State | free heap | largest DMA block |
|---|---|---|
| BLE API only | 80,867 | 31,732 |
| BLE API + AP + WebUI | 14,951 | **6,900** |
| after `webui -off` | 58,183 | 19,444 |

Quote the state a number was taken in. "Fully loaded" spans roughly 1.3 KB to 6.9 KB
of contiguous DMA depending on whether a station is attached, and the low-water mark
(`minEver`) dipped to 13,259 bytes of free heap across the cycle above.

### 7.2 BLE attacks: automatic transport swap

`blespam` handles this itself (`attack_commands.cpp:107`):

1. notifies the client `blespam: BLE control link suspended — reconnect over WiFi`,
   flushes with EOT;
2. `bleApiSuspend()` — frees the BLE stack while it is still healthy;
3. runs the spam;
4. restores the AP **only if** it was somehow torn down anyway (a
   `[BLE_SPAM] Restoring WiFi AP` line means the swap did not free enough — worth
   investigating, not ignoring);
5. `bleApiResume()` — fresh `setup()`, re-advertises as `Bruc`, re-registers the
   event sink.

**App handling:** on that suspend notice, switch to WiFi; when `Bruc` re-advertises,
switch back and reconcile via `lastEventId`. The v1.x advice — "don't enable
`ble api on` before a BLE attack" — is obsolete; the firmware does it for you.

**Verified 5/5 on 2026-07-29**, with the full RAMLOG sequence captured. The suspend
notice is delivered *before* the link drops; the AP-restore fallback never fired.
Measured across the swap:

| Stage | free heap | largest DMA block |
|---|---|---|
| `swap suspend-pre` | 80,907 | 31,732 |
| `swap suspend-post` | 144,591 | 55,284 |
| `swap resume-post` | 81,011 | 31,732 |

Suspend frees **63,684 bytes** — the "~62 KB" figure above, confirmed.

⚠️ **Two things the app must handle that were not previously documented:**

1. **The outage length varies enormously: 0.5 s, 1.8 s, 2.1 s, 2.4 s, 3.6 s and once
   11.9 s across five runs.** A reconnect timeout tuned to the ~2 s typical case will
   report a false failure. Allow at least ~12 s before treating the device as lost.
2. **`blespam`'s own telemetry is unreachable over BLE.** `state`, `ble_progress` and
   `ble_result` are emitted *while the BLE API is suspended*, so `pushWsEvent()` fans
   them out to `/ws` only. An app on BLE observes: suspend notice → silence →
   reconnect, with no way to see progress or outcome. To capture that telemetry the
   app must be on WiFi for the duration — which is precisely what the suspend notice
   is telling it to do.

### 7.3 WiFi attacks: no swap, control stays on BLE

`evilportal`, `karma`, `deauth`, `blesniffer` call
`cleanlyStopWebUiForWiFiFeature()` (`webInterface.cpp:66`), which stops the HTTP
server **and drops AP mode**. The WebUI does not come back on its own. BLE control
survives the whole attack (verified for `deauth` and Evil Portal, 2026-07-27).

**App handling:** expect HTTP to die; drive the attack over BLE; re-arm the WebUI
afterwards with `webui -bg` rather than asking the user to walk to the device.

### 7.4 Known-good / known-bad

- ✅ BLE API + WiFi AP + WebUI can all be *up* simultaneously — but see §1: the
  margin is ~15 KB and one associated station consumes it. Treat simultaneous
  operation as a transition state, not a steady state.
- ✅ HTTP (`/systeminfo`, `/getscreen`, `/cm`), `/ws`, and Bearer-auth 401s all
  work while the BLE API is active.
- ✅ `Bruc` visible in BLE scans from a PC. An iPhone 8 did not see it — likely iOS
  BLE privacy filtering. **UNVERIFIED** on other iOS devices.
- ❌ BLE API + a BLE attack cannot coexist; handled by §7.2's swap.

### 7.5 Diagnostics

`RAM_LOG()` stage markers are compiled in with `-D ENABLE_RAM_LOGGING` and mirrored
to UART0 (the native USB-CDC `Serial` on this board usually reaches nobody). The
periodic **sampler is separately opt-in** via `-D ENABLE_RAM_SAMPLER=1` because its
4 KB task stack comes out of internal DRAM — larger than the ~2.5 KB fully-loaded
margin it would be measuring (`ram_profile.h:26`).

Bench scripts live in `tools/ble_spike/` (`spike_transport.py`, `spike_events.py`,
`spike_swap.py`, `spike_suspend.py`, `probe_verbs.py`, `heap_poll.py`) — they
exercise the same transport semantics the app will, from a Linux laptop over BlueZ,
without an app build. See `tools/ble_spike/README.md`.

---

## 8. Not built

Listed so nobody plans against them:

- `cred`, `host`, `ap`, `packet` event frames.
- `/ws` `{cmd:"subscribe", since:…}` and any server-side event replay.
- `deauth <target>` targeting — the argument is accepted and discarded.
- `state` frames for any verb other than `evilportal` and `blespam`.
- Non-blocking execution of the menu-dispatcher verbs.
- A headless `evilportal` — no duration, no remote stop, ends only on the device
  (§5.3).
- Runtime hardware probing behind `/systeminfo` `capabilities` (§3.1) — the flags
  describe the board profile, not what is fitted.
- Any remote-abort path over BLE. `POST /cm cmnd=nav esc` is the only remote stop,
  and it is code-verified only (§5.2).
