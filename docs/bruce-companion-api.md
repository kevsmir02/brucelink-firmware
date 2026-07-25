# Bruce Companion App — API Contract

**Source of truth:** `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md` §5. This document is the verbatim contract the `bruce-companion-app` repo vendors. Bump the version line below whenever this contract changes.

**Contract version:** 1.0 (initial — matches firmware commit after Task 11)

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

| Verb | One-tap? | Telemetry bucket | Notes |
|------|----------|------------------|-------|
| `ble api on\|off` | yes | control plane | toggles BLE_API GATT server |
| `evilportal <ssid> <ch> [template]` | yes | 🥇 live & programmatic | verify-cmd: poll portal-AP creds endpoint |
| `blespam <type> <count>` | yes for fastpair_*; `menu` opens TFT | 🥈 TFT-encoded or /ws ble_progress | types: fastpair_regular, fastpair_fun, fastpair_prank, fastpair_custom, menu |
| `karma` | menu-dispatcher | 🥉 fire-and-forget | opens TFT menu |
| `deauth [<target>]` | menu-dispatcher (target ignored in v1) | 🥉 fire-and-forget | opens TFT menu |
| `blesniffer` | menu-dispatcher | 🥈 | opens BLE Suite menu |
| `ap_info` | yes (display on TFT) | 🥈 | shows current AP info on TFT |
| `reverseshell` | yes | 🥉 | starts reverse shell listener |
| `pwngrid` | yes | 🥉 | starts pwnagotchi/Brucegotchi |

## `/ws` event frames shipped

- `state` — `{type:'state', device_state:'idle|portal|ble_spam|…'}`
- `ble_progress` — `{type:'ble_progress', msg:string}`
- `ble_result` — `{type:'ble_result', success:boolean, msg:string}`
- `log` — `{type:'log', line:string, level?'info'|'warn'|'err'}`
- Plus the initial connect ack: `{id:0, type:'state', device_state:'idle'}`
