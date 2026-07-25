# Bruce Companion App — API Contract

**Source of truth:** `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md` §5. This document is the verbatim contract the `bruce-companion-app` repo vendors. Bump the version line below whenever this contract changes.

**Contract version:** 1.3 (blespam auto-recover verified — WiFi AP restarts after spam, all 7 /ws frames captured)

---

## 5. API Specification (firmware↔app contract)

Pinned here. Copied to `docs/bruce-companion-api.md` at implementation time.

### 5.1 Transport
- Wi-Fi (Bruce AP or shared LAN), HTTP REST + WebSocket.
- Auth: cookie `BRUCESESSION=<token>` after `POST /login`. App also sends `Authorization: Bearer <token>` (patched into `checkUserWebAuth` `webInterface.cpp:171` to accept bearer in addition to cookie — ~3 lines, ships with the patch file).
- mDNS: `http://<host>.local/` discovery.

### 5.2 Existing endpoints (unchanged, reused)
| Method | Path | Purpose |
|---|---|---|
| POST | `/cm?cmnd=<verb>` | Universal command trigger — fires any registered CLI verb |
| GET | `/getscreen` | Binary TFT draw-log pull |
| GET | `/systeminfo` | JSON system info (extended, see §5.4) |
| GET | `/listfiles?folder=&fs=` | Directory listing |
| GET | `/file?name=&action=&fs=` | download/image/delete/create/createfile/edit |
| POST | `/edit` | write file |
| POST | `/upload` | upload file |
| POST | `/rename` | rename file/folder |
| POST | `/login` `{user,pwd}` | → `Set-Cookie BRUCESESSION=`, also returns `{token}` JSON |
| GET | `/logout` | invalidate session |
| GET | `/reboot` | `ESP.restart()` |
| WS | `/ws` | Event stream (new — §5.5) |

### 5.3 New CLI verbs (added by `attack_commands.cpp`, §4.3)
```
evilportal <ssid> <ch> [template]      payload cred stream via §5.5 (or portal-AP poll fallback)
karma
deauth <target|ssid>
blespam <type> <count>                types: fastpair|ibeacon|apple|android|windows|ninebot
blesniffer
reverseshell
pwngrid
ble api on|off
ap_info <ssid>
```

### 5.4 Extended `/systeminfo`
```json
{
  "BRUCE_VERSION": "dev",
  "SD": {"free":"…","used":"…","total":"…"},
  "LittleFS": {"free":"…","used":"…","total":"…"},
  "capabilities": {
    "usb_as_hid": true, "has_screen": true, "lite_version": false,
    "has_cc1101": false, "has_nrf24": false, "has_pn532": false,
    "has_gps": false, "has_ir": false, "has_fm": false, "has_eth": false,
    "has_buzz": true, "has_rgb_led": true, "has_mic": true
  },
  "battery_pct": 73, "charging": false,
  "wifi_mode": "STA", "ip": "192.168.1.42",
  "free_heap": 142336, "psram": true
}
```
Capability flags are read from compile-time `#define`s the patch exposes; the app uses them to grey-out absent-feature buttons.

### 5.5 WebSocket event frames `/ws`
Server pushes JSON lines; client sends `{cmd:"subscribe", since:<lastEventId>}` on connect. Frame union:

```ts
type EventFrame =
  | { id:number, type:'cred',     ssid:string, fields:{[k:string]:string}, valid?:boolean, ts:number }
  | { id:number, type:'ble_progress', msg:string, color?:string, count?:number }
  | { id:number, type:'ble_result',   success:boolean, msg:string }
  | { id:number, type:'host',     mac:string, ip:string, vendor?:string }
  | { id:number, type:'ap',       ssid:string, bssid:string, ch:number, rssi:number, enc:string }
  | { id:number, type:'packet',   ch:number, rssi:number, len:number, type:string }
  | { id:number, type:'log',      line:string, level?:'info'|'warn'|'err' }
  | { id:number, type:'state',    device_state:string };  // idle|attacking|portal|sniffer|…
```

**Default-shipped hooks** (in the patch file): `ble_progress`/`ble_result` (from `showAttackProgress`/`showAttackResult`), `log` (from `handleSerialCommands` CLI output forward), `state` (from a small device-state setter). **Optional hooks** (time-permitting): `cred` (Mission 1 upgrade), `host`/`ap`/`packet` (Mission 3 upgrade).

### 5.6 Evil Portal creds endpoint (existing, used unless the `cred` `/ws` hook is shipped)
Poll `GET http://<portal.softAPIP>/<getCredsEndpoint>` (default configurable via `bruceConfig.evilPortalEndpoints`). Parse the returned HTML `<ol>` of `key: value<br>` lines into `EventFrame.cred`-shaped objects on the app side. Fallback CSV pull via `/file` after mission end.

---

## Verbs shipped by the patch

| Verb | Blocks serial? | One-tap? | Telemetry | Notes |
|------|---------------|----------|-----------|-------|
| `ble api on\|off` | no | yes | control plane | toggles BLE_API GATT server |
| `evilportal <ssid> <ch> [template]` | yes (monopolizes radio) | yes | 🥇 live & programmatic | gateway defaults to 192.168.4.1 (phone captive-portal compat); poll portal-AP creds endpoint or /ws cred hook |
| `blespam <type> <count>` | no (returns after spam cycle) | yes for fastpair_* | 🥈 /ws ble_progress | types: fastpair_regular, fastpair_fun, fastpair_prank, fastpair_custom, menu |
| `karma` | **YES — blocks** | menu-dispatcher | 🥉 fire-and-forget | opens TFT menu; user must dismiss on device |
| `deauth [<target>]` | **YES — blocks** | menu-dispatcher (target ignored in v1) | 🥉 fire-and-forget | opens TFT menu; user must dismiss on device |
| `blesniffer` | **YES — blocks** | menu-dispatcher | 🥈 | opens BLE Suite menu; user must dismiss on device |
| `ap_info` | **YES — blocks** | one-tap but blocks | 🥈 | shows AP info on TFT; blocks until user dismisses. Only works in STA mode (not AP mode) |
| `reverseshell` | **YES — blocks** | one-tap | 🥉 | starts reverse shell listener; blocks serial task |
| `pwngrid` | **YES — blocks** | one-tap | 🥉 | starts pwnagotchi/Brucegotchi; blocks serial task |

### §5.7 Blocking Verbs (critical for app design)

**The `/cm` endpoint queues commands on a depth-2 FreeRTOS queue** (`cmdQueue`). The serial-commands task processes them one at a time. Verbs marked "YES — blocks" above enter a `loopOptions()` or equivalent blocking loop on the TFT that **does not return until the user interacts with the device's physical buttons**. While blocked:

- The serial task is occupied → the `cmdQueue` fills up → **all subsequent `/cm` commands return HTTP 400** ("command failed") until the device-side loop is dismissed.
- The `/ws` stream stays alive (the AsyncWebServer runs on a separate task) but no new `log`/`state` frames are pushed for the blocked command.

**App design implications:**
- Non-blocking verbs (`ble api`, `blespam`, `evilportal`) are safe to fire-and-forget from the app — they return immediately or enter autonomous loops that don't block the serial task.
- Blocking verbs should be presented in the app with a **"Requires on-device interaction"** badge, and the app should **not** send any subsequent commands until the device returns to idle (detectable via `/ws` `state` frame `device_state:"idle"` or by polling `/systeminfo`).
- **Workaround for future patch:** run blocking verbs in a dedicated FreeRTOS task so the serial task stays free — not in this patch.

### §5.8 `/cm` HTTP method

**`/cm` accepts POST only** (registered as `server->on("/cm", HTTP_POST, ...)` at `webInterface.cpp:543`). GET requests return 404. The companion app must use `POST /cm?cmnd=<verb>`.

## `/ws` event frames shipped

- `state` — `{type:'state', device_state:'idle|portal|ble_spam|…'}`
- `ble_progress` — `{type:'ble_progress', msg:string}`
- `ble_result` — `{type:'ble_result', success:boolean, msg:string}`
- `log` — `{type:'log', line:string, level?:'info'|'warn'|'err'}`
- Plus the initial connect ack: `{id:0, type:'state', device_state:'idle'}`

**Verified on live device:** `blespam fastpair_regular 3` produces a 7-frame sequence: `log` (COMMAND) → `state` (ble_spam) → `ble_progress` (Starting) → `ble_progress` (Sent N) → `ble_progress` (completed) → `state` (idle) → `log` (Result: TRUE).

### §5.9 Hybrid BLE+WiFi Coexistence (verified on hardware)

**BLE API GATT server auto-starts at boot** (`main.cpp:setup()` calls `enableBLEAPI()` before WiFi AP init) so the BT controller grabs its ~15KB internal DMA block first. The device advertises as "Bruc" from the moment it boots.

**Verified coexistences (live on smoochiee-board):**
- ✅ BLE API GATT server + WiFi AP + Web UI HTTP server (all three simultaneously, indefinitely)
- ✅ HTTP endpoints (`/systeminfo`, `/getscreen`, `/cm`) work while BLE API is active
- ✅ `/ws` event stream works while BLE API is active
- ✅ Bearer auth (401 negatives) works while BLE API is active
- ✅ "Bruc" is visible in BLE scans (confirmed on PC; iPhone 8 did not see it — likely iOS BLE privacy filtering, not a firmware issue)

**Known conflicts (documented for app design):**
- ⚠️ **`blespam` may briefly drop the WiFi AP.** `spamFastPairPopups` calls `BLEStateManager::initBLE` → `radioHasMemForBle()` which is a **crash guard** — if internal DMA < 15KB, it tears down WiFi to free memory for the BT controller (PSRAM can't back BT DMA; this is a hardware constraint, not a bug). The firmware **auto-recovers**: after the spam finishes and BLE deinit's (freeing DMA), the AP is restarted via `WiFi.mode(WIFI_AP)` + `_setupAP()`. **App handling:** if WiFi drops mid-spam, show "Wi-Fi radio busy — reconnecting…" and auto-reconnect. Verified on hardware: all 7 `/ws` frames captured + WiFi returned to 200 within seconds.
- ⚠️ **WiFi attacks monopolize the radio.** `evilportal`, `karma`, `deauth`, `sniffer` call `cleanlyStopWebUiForWiFiFeature()` which kills the HTTP server. After the attack ends, the Web UI must be manually restarted on the device. During the attack, only BLE control (GATT serial) works.
- ⚠️ **BLE API GATT server + BLE attacks don't coexist.** `blespam`/`blesniffer` re-init the NimBLE stack, conflicting with a running BLE API server. **App mitigation:** don't enable `ble api on` before a BLE attack; use WiFi as the control path for BLE attacks. BLE API is only useful as fallback during WiFi attacks (when HTTP is dead).

### §5.10 BLE GATT Serial (control transport)

The BLE API GATT server exposes a serial-over-BLE channel:

| Item | Value |
|---|---|
| Service UUID | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` |
| Serial Characteristic UUID | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` |
| Battery Service UUID | `0x180F` (standard) |
| Battery Characteristic UUID | `0x2A19` (READ\|NOTIFY, updates/min) |
| Device Name | `Bruc` |
| Serial char properties | READ \| NOTIFY \| WRITE |

**Protocol:** Write newline-terminated CLI commands to the serial characteristic; receive CLI output via NOTIFY. This is the **same `SerialCli::parse` command bus** as `/cm` — every verb registered through `createAttackCommands` + all existing CLI commands work over BLE serial identically.

**Companion app v1 transport strategy (hybrid):**
1. **Primary: WiFi** (`POST /cm`, `/ws`, `/getscreen`, `/systeminfo`, file endpoints) — used when WiFi AP is available
2. **Fallback: BLE GATT serial** — used when WiFi drops (during a WiFi attack) or when the device is not running its AP
3. **Switch logic:** app detects WiFi loss (HTTP timeout) → switches to BLE serial for stop/status commands → when WiFi returns (detected via BLE serial `state` frame or periodic probe), switches back to WiFi for bulk data
