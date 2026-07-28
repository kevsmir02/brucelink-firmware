# Bruce Companion App — API Contract

**Source of truth:** this file, audited against the firmware tree. Where any
earlier design note disagrees with what is written here, the code wins. See
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) for *why* the firmware was changed;
this document covers *what the interface is*.

**Contract version:** 2.0 — audited against `623d4d26` (2026-07-28).
Fork point from upstream Bruce: `59e83bfb` (2026-07-23); companion work starts at
`373fb5d8` (2026-07-25). Bump this line whenever the contract changes.

**How to read this doc.** Every claim below is either code (file:line given) or a
measurement (device + date given). Anything not yet verified on hardware is marked
**UNVERIFIED**. Nothing here is aspirational — planned-but-unbuilt items live in
§7.

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
API + WiFi AP + WebUI the board has ~15 KB free, and a single associated station
drives that to a few hundred bytes, at which point notifies truncate and HTTP dies
(`src/core/serial_commands/wifi_commands.cpp:58`). Use `webui -off` to hand the
WiFi memory back when done with bulk transfer, `webui -bg` to bring it back.

---

## 2. BLE GATT serial (primary)

| Item | Value | Source |
|---|---|---|
| Advertised name | `Bruc` | `ble_api.cpp:67` |
| Service UUID | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` | `BLESerialService.cpp:66` |
| CLI characteristic | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` — READ \| NOTIFY \| WRITE | `BLESerialService.cpp:69` |
| Event characteristic | `d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9` — READ \| NOTIFY | `BLESerialService.cpp:79` |
| Battery service | `0x180F` / `0x2A19` (READ \| NOTIFY) | `BatteryService.cpp` |
| Response terminator | `0x04` (EOT) | `BLESerialService.h:22` |

### 2.1 Framing

**Commands** are newline-terminated writes to the CLI characteristic. The firmware
buffers RX in a 512-byte ring and only parses on a complete line
(`serialcmds.cpp:57` uses `hasLine('\n')`, not `available()`), so a command split
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
  mode. For the AP address use the known `192.168.4.1`.
- `has_pn532`, `has_fm`, `has_eth` are **hardcoded `false`**
  (`system_info.cpp:47,58,59`) regardless of the hardware. Do not trust them.
- Every other capability flag is a real compile-time `#define` check.

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

| Verb | Blocks serial? | Telemetry | Notes |
|---|---|---|---|
| `ble api on\|off` | no | — | toggles the GATT server; persists across reboot (§5.3) |
| `evilportal <ssid> <ch> [template]` | yes (monopolizes radio) | `state` | defaults: ssid `Free Wifi`, ch `6` (out-of-range → 6); gateway forced to `192.168.4.1` for phone captive-portal compat |
| `blespam <type> <count>` | no | `state`, `ble_progress`, `ble_result` | types in §5.1; count < 1 → 10 |
| `systeminfo` | no | — | same JSON as `GET /systeminfo`; the way to read it over BLE |
| `free` | no | — | one-line heap report incl. **largest contiguous DMA block**, which is what actually gates radio init |
| `webui -off` | no | — | stops the WebUI **and its AP**; frees the memory for BLE |
| `webui -bg` | no | — | starts the WebUI and returns instead of holding the screen until ESC |
| `karma` | **YES** | — | opens the TFT menu; needs on-device dismissal |
| `deauth [<target>]` | **YES** | — | opens the TFT menu; **`target` is parsed but ignored** (`attack_commands.cpp:152`) |
| `blesniffer` | **YES** | — | opens the BLE Suite menu; needs on-device dismissal |
| `ap_info` | **YES** | — | STA mode only; blocks until dismissed |
| `reverseshell` | **YES** | — | starts the listener; blocks the serial task |
| `pwngrid` | **YES** | — | starts Brucegotchi; blocks the serial task |

`webui`, `systeminfo` and `free` live in `wifi_commands.cpp` / `util_commands.cpp`,
not `attack_commands.cpp`.

### 5.1 `blespam` types

Two engines behind one verb (`attack_commands.cpp:74`):

- **FastPair popup engine:** `fastpair_regular`, `fastpair_fun`, `fastpair_prank`,
  `fastpair_custom`
- **Generic spam engine** (`bleSpamAttackTypeFromName`, `ble_spam.cpp:1592`):
  `apple`, `android`, `ibeacon`, `samsung`, `windows` (alias `swiftpair`),
  `random` (alias `all`)
- **`menu`** — opens the interactive on-device UI and drives its own radio
  lifecycle (no transport swap)

There is no bare `fastpair` and no `ninebot`, despite the v1.x contract listing
both. Unknown types are rejected **before** any radio teardown, so a typo does not
flap the AP.

### 5.2 Blocking verbs

`/cm` queues onto a **depth-2** FreeRTOS queue (`serialcmds.cpp:91`) drained one at
a time by the serial task. Verbs marked **YES** enter a `loopOptions()` on the TFT
that does not return until the user presses a physical button. While blocked:

- the queue fills → all further `/cm` calls return **HTTP 400**;
- `/ws` stays alive (AsyncWebServer is a separate task) but no new frames are
  pushed for the blocked command.

App handling: badge these verbs "Requires on-device interaction" and send nothing
further until `device_state` returns to `idle`. Note the blocking verbs do **not**
emit a `state` frame at all, so the only reliable idle signal is the next `/cm`
succeeding, or a `free`/`systeminfo` round-trip over BLE.

A future patch could run them in a dedicated task; not done.

### 5.3 BLE API persistence and boot

`ble api on|off` (and the Config-menu toggle — both go through `enableBLEAPI()`)
writes `bleApiAutoStart` to the config file (`settings.cpp:1690`). At the **end** of
`setup()`, after WiFi init, the firmware re-arms the GATT server if that flag is set
(`main.cpp:581`). Default is `0` — a device that has never been told `ble api on`
boots without it.

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

Measured, fully loaded (BLE API + AP + WebUI): largest DMA block **1,332 bytes** —
the guard cannot pass. After `bleApiSuspend()` releases ~62 KB: **32,756 bytes** —
the guard passes on its first check and never touches WiFi.

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
