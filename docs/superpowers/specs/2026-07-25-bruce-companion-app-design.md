# Bruce Companion App — Design Spec

**Date:** 2026-07-25
**Status:** Draft (pending user review)
**Owner:** (student capstone)
**Source of truth:** this file. The app repo (`bruce-companion-app`, to be created separately) consumes this as its API contract with the firmware.

---

## 1. Goal & Context

Build a **mobile companion app** for the `bruce-companion` ESP32-S3 Bruce firmware running on the `smoochiee-board` target. The app lets a user **remotely trigger and observe device features from their phone**, as **one-tap actions** — explicitly **not** a mirror of the on-device TFT menu or the existing Web UI's D-pad remote.

### Why this exists
A student capstone. The student saw Bruce's existing Web UI (join the device AP, drive the TFT via on-screen D-pad) and asked: "why can't that be a real app, where I tap a labeled button and the device executes the feature?" The capstone is graded on **breadth of working features** — how many device features can actually be triggered and observed from the phone, demoed live.

### Out of scope (explicit anti-goals)
- **Not** a TFT screen mirror (we will *optionally* decode the TFT draw-log for one telemetry path, but the app's face is labeled buttons, not a rendered LCD).
- **Not** a re-skin of the existing Web UI's menu/drill-down D-pad remote (Concept B, rejected — it grades as "a reshaped mirror").
- **Not** a substitute for on-device interaction — Bruce keeps its physical buttons and TFT; the app is a parallel control surface.

### Target hardware (verified against `boards/smoochiee-board/`)
- ESP32-S3-N16R8 (16 MB Flash, 8 MB OPI PSRAM), `custom_16Mb.csv` partitions.
- 1.47" ST7789 IPS LCD, 170×320, rotation 1.
- 5 tactile buttons (Up/Down/Left/Right/Select). `HAS_5_BUTTONS`, `BTN_ACT=LOW`.
- Peripherals attached: **none external.** CC1101/NRF24 SPI CS lines driven `HIGH` at boot (`interface.cpp:25-29`) → chips deselected. PN532, GPS, IR LED, FM, Ethernet all absent.
- Active build flags that matter: `USB_as_HID=1` (BadUSB usable), `HAS_SCREEN=1`, `!LITE_VERSION` (full feature set incl. BLE_API, bjs interpreter), `BUZZ_PIN=44` + `MIC_INMP441` (audio usable), `HAS_RGB_LED` WS2812B×16, `XPOWERS_CHIP_BQ25896` (battery telemetry usable). SD card slot on `SDCARD_CS=3`.

---

## 2. Success Criteria

Graded on **breadth of working features**: how many device features can be triggered and observed from the phone, live, in a panel demo.

- **Primary (must):** ~30 device features runnable from the phone as one-tap actions across the Manual Console, with the three headline missions fully demonstrable live against a victim device.
- **Primary (must):** the Evil Portal Campaign mission shows captured credentials appearing on the phone **in real time** as a victim logs in. This is the money shot.
- **Secondary (should):** BLE Chaos mission shows a live counter (popups/beacons sent) decoded from the device's screen-draw log while Bruce spams. If the WebSocket telemetry patch lands, this counter is fed by pushed events instead of screen-decode polling.
- **Secondary (should):** WiFi Recon Sweep mission shows live ARP scan results during its live stage and pulled-back sniffer output during its fire-and-forget stage.
- **Tertiary (nice):** BadUSB Drop and Script Runner demonstrate plug-and-execute and live script stdout respectively, from the Manual Console (not missions).
- **Demo setup (confirmed):** phone running the app + smoochiee board + a second phone/laptop as the Evil Portal victim + a target Bluetooth device for BLE Chaos. All three missions demoable live.
- **Out of scope for v1:** BLE as a control transport (we ship Wi-Fi-only for control; the existing BLE_API GATT server remains available but the app doesn't drive over it in v1), over-the-air app installers, multi-device pairing, **and** exposing features that require absent hardware (CC1101/NRF24/PN532/GPS/IR/FM/Ethernet → those are greyed out in the app UI by the board capability profile, not removed).

---

## 3. Concept: "Mission Playbook"

Two faces sharing one backbone:

1. **The face: 3 Missions** — preset orchestration sequences that chain real device features into a panel-grade story, each mission showcasing a different telemetry bucket (so the ESP32's single-radio limit becomes part of the narrative, not a bug).
2. **The backbone: Manual Console** — Concept A's categorized one-tap action grid covering **all** runnable smoochiee features. Missions are the attractor; the console is the safety net. Any feature not in a mission is still one tap away.

### Why this shape
A pure console (Concept A) wires the same ~30 features but reads to a panel as "a wall of buttons." Missions give the demo a story. The console guarantees breadth coverage and hedges the schedule: **C degrades to A** if any mission slips — ship the console + however many finished missions; you never lose the breadth. Confirmed direction after the A-vs-C walk-through.

### The three missions

**Mission 1 — Evil Portal Campaign** (telemetry bucket 🥇 live & programmatic)
- User picks a portal SSID and template (default "Free Wifi" + Google-login default HTML).
- App sends `evilportal <ssid> <ch> [template]` → firmware starts the Evil Portal.
- App instructs the user to join the portal AP from the **victim** device (the captive portal's own AP, gateway `WiFi.softAPIP`, typically `172.0.0.1`).
- App polls the portal's creds endpoint (default `<getCredsEndpoint>`, configurable in `bruceConfig.evilPortalEndpoints`) → captured logins stream into the mission status feed as cards (`email`, `password`, `ts`, `valid`), **live as the victim submits**.
- Mission ends on user stop, or auto after a configurable timeout. Final creds also pulled from `/BruceEvilCreds/<ssid>_creds.csv` on SD/LittleFS via the main Web UI `/file` route for a "complete haul" summary.
- **Firmware telemetry cost: zero.** The captive portal already runs its own `AsyncWebServer` (`evil_portal.cpp:22`) and exposes `creds_GET()` (`evil_portal.cpp:793`); CSV persistence already exists (`saveToCSV` `evil_portal.cpp:818`).

**Mission 2 — BLE Chaos** (telemetry bucket 🥈 live but TFT-encoded)
- User picks a spam type (FastPair popups / iBeacon / Apple alerts / Android alerts / Windows / Ninebot) and a duration/count.
- App sends `blespam <type> <count>` → firmware spam-cycles via `ble_spam.cpp`'s existing entry (driven by `showAttackProgress` / `showAttackResult` — `BLE_Suite.cpp:5933/5985`, 53 callers total).
- **Crucially, BLE attacks do *not* call `cleanlyStopWebUiForWiFiFeature`** (grep: that's only called from `wifi/*` modules), so the main Web UI `/cm`, `/ws`, and `/getscreen` stay up throughout the BLE attack.
- App polls `GET /getscreen` at ~5 Hz, decodes the TFT draw-log binary (see `tftLogger.cpp:150 getBinLog`), and renders the "Spammed N popups" text line into a live counter card. **OR**, if the optional `/ws` patch lands, `showAttackProgress` pushes `{type:'ble_progress', msg, count}` frames directly — cleaner, costs ~5 lines.
- Mission ends on stop or counter reaching target. TFT shows the cancel + result screen; app mirrors it.

**Mission 3 — WiFi Recon Sweep** (telemetry bucket 🥉 fire-and-forget)
- User picks a target SSID (or "all").
- Stages run sequentially on the app side (each stage = one `POST /cm`):
  1. **ARP scan** (`arp`) — LIVE: results come back via `/ws` (or `/cm` response) as `{type:'host', mac, ip}` rows rendered live.
  2. **AP info** (`ap_info <ssid>` — new thin verb) — returns channel, rssi, encryption; rendered as a result card.
  3. **Sniffer capture** (`sniffer`) — FIRE-AND-FORGET: this stage calls `cleanlyStopWebUiForWiFiFeature()` and Bruce's HTTP disappears. App shows "Capturing… (Wi-Fi radio busy)" until user taps stop → `nav esc` → Web UI restarts → app pulls the saved pcap-ish dump file via `/listfiles` + `/file?action=download` and shows a packet summary.
- This mission explicitly demonstrates the single-radio tradeoff: stage 3 is shown as a deliberate "Armed → Running → Done" beat, not a bug.

### Mission list rationale
Three missions chosen because they map 1:1 to the three telemetry buckets verified in the firmware source — each mission showcases one way the app copes with radio physics. No 4th mission: BadUSB Drop and Script Runner don't benefit from orchestration (they're already one-tap in the console), and 3 missions is the max polishable in 2 weeks.

---

## 4. Architecture

### 4.1 Two repos, one contract

- **`bruce-companion`** (this repo, firmware) — source of truth for the **API contract** with the app. The contract file lives here at `docs/bruce-companion-api.md` (generated from this spec's §5). Any firmware change that affects the contract updates this doc and is committed.
- **`bruce-companion-app`** (new, separate repo) — the RN/Expo app. Consumes the contract by checked-in copy (or git submodule reference at the app side: `vendor/bruce-companion-api.md`). Nothing else couples the repos.

**Why separate:** PlatformIO/Arduino/ESP-IDF build vs node/metro/Xcode/Android-SDK build have zero overlap and conflicting root configs (`platformio.ini` vs `package.json`/`metro.config.js`/`ios/`/`android/`). Co-locating pollutes firmware git history, trips CI, and splits one README. Standard split; Bruce's own existing web UI is embedded C, not a separate JS app — precedent for "companion UI = separate concern."

### 4.2 Mobile app tech stack (confirmed)

- **Expo + React Native + TypeScript** — one JS codebase, OTA updates, aligns with Bruce's MicroQuickJS/JS culture.
- HTTP: `axios` (cookie-jar support for `BRUCESESSION=` session).
- WebSocket: RN built-in `WebSocket` (consumes `/ws`).
- BLE (v2 stretch only): `react-native-ble-plx` (not wired in v1; the `/ws`/`/cm` over Wi-Fi is the v1 transport).
- State: `zustand` (skip Redux).
- Navigation: `expo-router` (file-based).
- Styling: `StyleSheet` primitives; NativeWind only if a designer adds Tailwind later.
- TFT draw-log decoder: a small hand-written TS module (`src/tft/decoder.ts`) that parses the binary `getBinLog` frames into drawable ops. Only the subset of ops the missions need (text + spinner + bar — `showAttackProgress` writes via `tft.print(...)`/`fillScreen(...)`) must decode; full ANSI-style render is not required.

### 4.3 Firmware patch (confirmed: ship it)

One new file in the firmware repo:

- **`src/core/serial_commands/attack_commands.cpp`** + header, registered in `src/core/serial_commands/cli.cpp::setup()` under `#if !defined(LITE_VERSION)`. Registers thin CLI verbs (each verb calls the *same entry function the corresponding menu item already calls* — the module code already exists; menus just invoke it):

| New verb | Calls (existing entry) | Telemetry bucket |
|---|---|---|
| `evilportal <ssid> <ch> [template]` | `EvilPortal(...)` constructor path + `loop()` | 🥇 live & programmatic |
| `karma` | karma_attack start entry | 🥉 fire-and-forget |
| `deauth <target\|ssid>` | deauther entry | 🥉 fire-and-forget |
| `blespam <type> <count>` | `ble_spam.cpp` selected spam entry | 🥈 TFT-encoded |
| `blesniffer` | `ble_sniffer.cpp` entry | 🥈 TFT-encoded |
| `reverseshell` | `reverseShell.cpp` entry | 🥉 fire-and-forget |
| `pwngrid` | `pwnagotchi.cpp` entry | 🥉 fire-and-forget |
| `ble api on\|off` | `enableBLEAPI()` (`settings.cpp:1672`) | (control plane) |
| `ap_info <ssid>` | wifi ap_info entry | 🥈 live |

### 4.4 Optional firmware telemetry patch (decide mid-build)

A WebSocket event stream + two small hooks:

- `/ws` route registered in `webInterface.cpp::configureWebServer()` via `AsyncWebSocket` (ESPAsyncWebserver bundles it).
- Hook `EvilPortal::credsController` to also emit `{type:'cred',...}` on every capture (cleans Mission 1 from poll-on-portal-AP to push-on-main-Web-UI — but Mission 1 already works without it; this is a quality bump).
- Hook `showAttackProgress`/`showAttackResult` (`BLE_Suite.cpp`) to also push `{type:'ble_progress'/'ble_result',...}` (cleans Mission 2 from `/getscreen` decode to pushed JSON — ~5 lines, big UX win).

**Default path:** ship the `/ws` stream + BLE hook (since Mission 2 polish is worth ~5 lines). Skip the EvilPortal hook (its existing portal-AP-poll path already works and is more impressive — "the phone joins the portal it just spawned"). If time gets tight, drop this entire block — Missions 1 and 3 work without it, Mission 2 degrades to `/getscreen` decode.

### 4.5 Existing firmware surface we reuse (verified, zero new work)

| Capability | Existing endpoint/verb | Where |
|---|---|---|
| Universal command trigger | `POST /cm?cmnd=<cli verb>` | `webInterface.cpp:543` → `parseSerialCommand` `serialcmds.cpp:15` |
| Virtual D-pad (used for stop) | `POST /cm?cmnd=nav <esc\|sel\|...> [ms]` | `webInterface.cpp:547-566` |
| Screen-draw log pull | `GET /getscreen` binary | `tft.getBinLog` `tftLogger.cpp:150` |
| File manager | `GET /listfiles` `GET /file?name=&action=&fs=` `POST /upload` `POST /edit` `POST /rename` | `webInterface.cpp:585-725` |
| System info | `GET /systeminfo` JSON (battery via BLE `0x2A19` separately) | `webInterface.cpp:461` |
| Auth | cookie `BRUCESESSION=`, `/login` `/logout`, max 5-10 sessions | `webInterface.cpp:409-447` + `config.cpp:854` |
| mDNS discovery | `host.local` | `webInterface.cpp:379 startMdnsResponder` |
| BLE remote-serial (v2 transport, kept available) | GATT `4371ec0b-…` / char `d555…`, toggled by `ble api on\|off` | `ble_api.cpp:22`, `BLESerialService.cpp:18`, `settings.cpp:1672` |
| Tier-1 verbs (one-tap today, no patch) | `sniffer`, `arp`, `listen`, `webui`, `wifi on/off/add`, `bu run_from_file`, `js`/`run`, `tone`, `play`, `tts`, `clock`, `screen brightness/color rgb/hex`, `reboot`, `sleep`, `poweroff`, `info`/`!`, `uptime`, `date`, `free`, `i2c`, `factory_reset`, `gpio mode/set/read`, `ls`/`cat`/`rm`/`md`, `crypto encrypt/decrypt`, `loader open <app>`, `optionsJSON`, `options run <n>` | `serial_commands/*_commands.cpp` |

### 4.6 App surfaces

```
Home (Mission tiles x3 + device status header)
├─ Mission: Evil Portal Campaign     [→ live creds feed]
├─ Mission: BLE Chaos                [→ live counter feed]
├─ Mission: WiFi Recon Sweep         [→ 3-stage live/fire-and-forget]
├─ Manual Console tab                [→ categories: WiFi · BLE · BadUSB · Scripts · Device · Files]
└─ Status header                     [battery %, IP/Bruce version, Wi-Fi mode, free heap, current state]
```

- Each Mission screen is a vertically scrolling **stage list** with live status cards per stage.
- The Manual Console is a categorised grid of buttons; a tap opens a bottom-sheet form for any required parameters (pre-filled with sane defaults), then fires `POST /cm?cmnd=<verb> <args>`.
- A persistent **Activity** bottom dock shows the live log stream from `/ws` (or decode of `/getscreen`) across both faces.
- A **Capabilities** banner at app launch pulls `/systeminfo` (extended in §5 to include capability flags) and greys-out features absent on the connected board (sourced from compile-time flags reported by the firmware). Keeps one app binary serving every Bruce board; smoochiee shows only its usable set.

### 4.7 Component boundaries (for isolation/testability)

- **`DeviceTransport` interface** — `exec(cmd): Promise<string>`, `stream(): AsyncIterable<EventFrame>`, `getScreen(): Promise<Uint8Array>`, `files()` namespace, `system(): SystemInfo`. One implementation in v1: `WifiTransport` (HTTP+WS+`/getscreen`). v2 adds `BleTransport`.
- **`Mission` model** — `Mission { id, title, stages: Stage[] }`; `Stage { id, label, verb, telemetry: 'live'|'tft'|'after', onComplete }`. Stage state machine runs in a `useMissionRunner` hook. Stages are independent units; each is runnable in isolation (lets you unit-test one stage without the others).
- **`Console` model** — flat list of `FeatureButton { id, category, label, verb, params: Param[] }`. Parameterized by a `Capabilities` object so unavailable features are filtered/disabled, not removed.
- **`TftDecoder`** — pure function `(bytes: Uint8Array) => DrawOp[]`. No side effects; trivial to unit-test with fixture byte arrays.
- **`/ws EventStream`** — typed `EventFrame` discriminated union (`{type:'cred'|'ble_progress'|'ble_result'|'host'|'log', ...}`).

### 4.8 Error handling and reconnect

- Wi-Fi loss during a mission: exponential backoff (1→2→4→8 s, max 30 s). For fire-and-forget stages this is invisible (device runs isolated; app reconnects on stop). For live stages, show "reconnecting…" overlay; `/ws` resumes with a monotonic `lastEventId` (added in the §4.4 patch).
- Bruce ESP32 reset mid-mission: app detects via `/ws` close + `/systeminfo` failure → offers "Restart mission?" with stage state preserved locally (zustand persist).
- Verb failure (CLI returns `400` or `false`): show inline error in the originating button's status card; do not abort sibling stages.
- Evil Portal victim-not-joining: app shows a "Waiting for victim…" card with a "Join `Free Wifi`" copy-SSID helper — the captive portal webserver handles the rest.

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

## 6. Two-Week Implementation Plan (summary; full plan via writing-plans skill)

| Days | Track | Output |
|---|---|---|
| 1-2 | Firmware patch | `attack_commands.cpp` (~8 verbs) registered in `cli.cpp`; flash smoochiee; verify each verb over `POST /cm` via `curl`. Extend `/systeminfo` + bearer auth. Commit + push. |
| 3 | Firmware telemetry | `/ws` route + `showAttackProgress`/`showAttackResult` hooks + `log`/`state` frames. Smoke test from a browser WS client. |
| 4-5 | App scaffold | New `bruce-companion-app` repo; Expo RN+TS; `DeviceTransport` interface + `WifiTransport`; first `POST /cm` round-trip from the phone; `/systeminfo` → status header. |
| 6-8 | Manual Console | All ~30 feature buttons across categories; bottom-sheet param forms; Activity dock wired to `/ws` `log`/`state`. |
| 9-11 | 3 Missions | Mission UI + `useMissionRunner` stage state machine; Mission 1 (Evil Portal creds poll); Mission 2 (BLE Chaos `/ws` counter, fallback `/getscreen` decode); Mission 3 (3-stage ARP→ap_info→sniffer+pull). |
| 12 | Live telemetry polish | TFT decoder for Mission 2 fallback; sniffer file pull for Mission 3; reconnect/`lastEventId` resume. |
| 13 | Visual polish + dry-run | Status header, mission card design, capability greying; student dry-runs with the victim device. |
| 14 | Buffer / fallback | Whatever slipped from days 9-12; if any mission unfinished, ship the console + finished missions. |

**Degradation path:** any unfinished mission is hidden from Home; the Manual Console carries the breadth rubric alone. C → A gracefully. Breadth never drops below ~30 feature buttons as long as days 1-2 + 6-8 land.

---

## 7. Testing

- **Firmware verbs:** smoke-test each new CLI verb via serial + `curl 'http://<bruce>local/cm?cmnd=<verb>'`; expected response text asserted in a small shell script in the firmware repo's `tools/` (not a C++ unit test framework — Bruce has none).
- **App `TftDecoder`:** unit test with fixture `getBinLog` byte arrays (captured once from a running device) → assert decoded op sequence. Pure function, no device needed.
- **App `useMissionRunner`:** unit test the stage state machine with a mocked `DeviceTransport` returning scripted `EventFrame` streams; assert stage transitions + error recovery.
- **App transport contract:** an integration test that points the app at a local mock server (Express) implementing §5 fixture responses, asserted against the contract types.
- **Manual dry-run:** the student + victim device end-to-end, day 13. Captures the actual demo UX.

No firmware C++ test framework is introduced (Bruce has none; the patch's verbs each delegate to already-existing module code that the menus already exercise).

---

## 8. Open Questions (resolve during writing-plans)

1. Does the Evil Portal mission prefer its own captive-AP poll (impressive — "phone joins the portal it spawned") or the `/ws` `cred` push (simpler code, but Mission 1 loses the "join the AP" story)? *Default: poll path; `/ws cred` hook only if time remains.*
2. Should the `/ws` channel authenticate via the same `BRUCESESSION` cookie (works for WS subprotocol via header on connect) or a separate token query param? *Default: cookie + bearer-equivalent query param `?token=`.*
3. Mission 3 `ap_info` verb — does the firmware have a clean entry to expose, or do we wrap `loopOptions` into a one-shot like the menu does? *Resolve in writing-plans: read `src/modules/wifi/ap_info.cpp` entry point.*
4. TFT draw-log decode subset: confirm which op codes `showAttackProgress` actually emits (likely `FILLSCREEN` + per-char text draws via `tft.print`). The `TftDecoder` only needs to handle that subset for Mission 2 fallback. *Resolve in writing-plans: trace `showAttackProgress`'s drawing calls.*

---

## 9. Glossary

- **Bruce** — the ESP32-S3 pentest firmware in this repo.
- **smoochiee-board** — the capstone's physical target board (`env:smoochiee-board`).
- **TFT draw-log** — the binary log of TFT drawing operations captured by `tft_logger` and exposed via `GET /getscreen`. Decoded op-by-op to reconstruct what's on the LCD.
- **Telemetry bucket** — classification of how an app-visible result is delivered: 🥇 live & programmatic (existing endpoint), 🥈 live but TFT-encoded (poll `/getscreen` or push via `/ws`), 🥉 fire-and-forget (device runs isolated, results pulled after).
- **Capabilities** — compile-time flags reported via extended `/systeminfo` that tell the app which hardware modules are present on the connected board.
- **Mission** — a preset orchestration of multiple device features into a panel-grade story; each mission maps to one telemetry bucket.
- **Manual Console** — Concept A's categorized one-tap grid; safety net under the missions.