# Bruce Companion App — API Contract

**Source of truth:** `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md` §5. This document is the verbatim contract the `bruce-companion-app` repo vendors. Bump the version line below whenever this contract changes.

**Contract version:** 1.1 (updated after live-device verification — see §5.7 Blocking Verbs)

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
