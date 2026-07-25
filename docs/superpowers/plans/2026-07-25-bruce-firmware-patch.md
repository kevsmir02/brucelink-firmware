# Bruce Firmware Patch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ~9 thin CLI verbs + a `/ws` event stream + extended `/systeminfo` to the Bruce firmware so the companion mobile app can trigger every runnable smoochiee-board feature with one tap and observe results live.

**Architecture:** Each new verb calls the *existing* module entry function the corresponding on-device menu already invokes — no attack logic is rewritten. A new `attack_commands.cpp` registers the verbs through the existing `SerialCli` bus, which automatically exposes them over USB serial, the BLE_API GATT serial characteristic (once enabled), and the Web UI `POST /cm` endpoint. A new `/ws` AsyncWebSocket routes pushes from small hooks added at `showAttackProgress`/`showAttackResult` and the CLI output forwarder.

**Tech Stack:** C++17 (Arduino-ESP32 3.3.x), PlatformIO (`env:smoochiee-board`), ESPAsyncWebServer (`AsyncWebSocket` bundled), NimBLEDevice, SimpleCLI. No C++ test framework introduced (Bruce has none); verification is via `curl` smoke tests against a flashed device + a small shell script.

## Global Constraints

Copied verbatim from `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md`:

- **Target board:** `env:smoochiee-board` (ESP32-S3-N16R8, 16 MB Flash, 8 MB OPI PSRAM, `custom_16Mb.csv` partitions).
- **Active build flags on smoochiee:** `USB_as_HID=1`, `HAS_SCREEN=1`, `HAS_5_BUTTONS`, `!LITE_VERSION`, `BUZZ_PIN=44`, `MIC_INMP441`, `HAS_RGB_LED`, `XPOWERS_CHIP_BQ25896`.
- **Patch gating:** every new verb and every `/ws` hook is wrapped `#if !defined(LITE_VERSION)`. Other Bruce boards keep working unchanged.
- **No rewrites of attack logic:** verbs call existing menu entry functions. If an entry function needs a parameter it doesn't take, the verb calls its menu-dispatcher form and we document that the user steers the rest on the TFT — we do NOT refactor menu code to be headless in this plan.
- **Build command:** `pio run -e smoochiee-board`. **Flash command:** `pio run -e smoochiee-board -t upload` (or `pio device run -e smoochiee-board` for upload+monitor).
- **mDNS host:** `bruce.local` (set by `startMdnsResponder`). Substitute your device IP if mDNS isn't resolving.
- **Default webui creds:** `bruceConfig.webUI.user` / `.pwd` — read from config; set via existing `GET /wifi?usr=&pwd=` if unset. The smoke tests below assume you've already logged in once and have `BRUCESESSION=<token>` from `POST /login`.

---

## File Structure

### New files
- `src/core/serial_commands/attack_commands.h` — declares `void createAttackCommands(SimpleCLI *cli);` (plus the `bleApiIsEnabled()` getter forward-decl if needed).
- `src/core/serial_commands/attack_commands.cpp` — the verb callbacks + `createAttackCommands` body.
- `src/core/wifi/ws_events.h` — declares `void beginWsServer(AsyncWebServer *server);`, `void pushWsEvent(const String &type, const String &jsonPayload);`, `void pushWsLog(const String &line, const char *level);`, `void setDeviceState(const String &state);`, `String getDeviceState();`.
- `src/core/wifi/ws_events.cpp` — the `AsyncWebSocket ws("/ws")` instance, per-client `lastEventId` tracking, monotonic id counter, event push loop.
- `tools/smoke_test_attacks.sh` — curl-runs every new verb + asserts expected HTTP status & visible device behavior (manualobserve step).
- `docs/bruce-companion-api.md` — generated API-contract document (the spec §5 content, frozen here as the source of truth the app repo vendors).

### Modified files
- `src/core/serial_commands/cli.cpp` — `#include "attack_commands.h"` + `createAttackCommands(&_cli);` inside the `#if !defined(LITE_VERSION)` block of `SerialCli::setup()`.
- `src/core/settings.h` — add `bool bleApiIsEnabled();` declaration next to `void enableBLEAPI();` (line 111).
- `src/core/settings.cpp` — add `bool bleApiIsEnabled() { return ble_api_enabled; }` next to `enableBLEAPI()` (line 1672).
- `src/core/wifi/webInterface.cpp:171` (`checkUserWebAuth`) — accept `Authorization: Bearer <token>` before the cookie check.
- `src/core/wifi/webInterface.cpp:396` (`configureWebServer`) — call `beginWsServer(server);` after `server->begin();` (or just before — see task).
- `src/core/wifi/webInterface.cpp:461` (`/systeminfo` route) — extend the JSON body with `capabilities` + `battery_pct` + `charging` + `wifi_mode` + `ip` + `free_heap` + `psram`.
- `src/modules/ble/BLE_Suite.cpp:5933` (`showAttackProgress`) + `:5985` (`showAttackResult`) — add one `pushWsEvent` call each, gated `#if !defined(LITE_VERSION)`.
- `src/core/serialcmds.cpp:37` (`handleSerialCommands`) — after `serialCli.parse(...)`, forward the CLI response line to `/ws` as a `log` frame; on attack-verb start, push a `state` frame.

---

## Task 1: `ble api on|off` verb + scaffold (`attack_commands.cpp`)

This is the smallest real verb (toggles the BLE_API GATT server that the app will use as its v2 transport and that already exists at `ble_api.cpp:22`). It also establishes the scaffold: the new file, the registration line in `cli.cpp`, and the `bleApiIsEnabled()` getter the verb needs for explicit on/off semantics.

**Files:**
- Create: `src/core/serial_commands/attack_commands.h`
- Create: `src/core/serial_commands/attack_commands.cpp`
- Modify: `src/core/serial_commands/cli.cpp:34` (inside `SerialCli::setup()`'s `#if !defined(LITE_VERSION)` block, line ~52 where `createInterpreterCommands(&_cli);` lives)
- Modify: `src/core/settings.h:111`
- Modify: `src/core/settings.cpp:1672`

**Interfaces:**
- Consumes: `void enableBLEAPI();` (declared `settings.h:111`, defined `settings.cpp:1672`, toggles `ble_api_enabled`). SimpleCLI API: `cli->addCompositeCmd("ble")`, `cmd.addCommand("api", cb)`, `cbCmd.addPosArg("state", "on")` — see existing `power_commands.cpp:21-25` for the one-verb pattern.
- Produces: `void createAttackCommands(SimpleCLI *cli);` (signature later tasks rely on) and `bool bleApiIsEnabled();` (later tasks may read it; Task 7 uses it to push the initial `/ws` state frame).

- [ ] **Step 1: Write the failing test**

The smoke test is a `curl` invocation. Before this task, `ble api` is an unknown command.

```bash
# assuming BRUCESESSION=<token> already set from a prior POST /login
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ble%20api%20on'
```

Expected PRE-implementation response: `400` with body `command failed, check the serial log for details` (the CLI rejects unknown verbs — `ble` is not registered).

- [ ] **Step 2: Run test to verify it fails (baseline)**

Run:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ble%20api%20on'
```
Expected: `400` and the body above. Confirm in the device serial log: `COMMAND: ble api on` followed by an Unknown-command error.

- [ ] **Step 3: Write minimal implementation**

a. Add the getter to `src/core/settings.h` at line 111 (next to the existing declaration):

```cpp
void enableBLEAPI();
bool bleApiIsEnabled();
```

b. Add the getter body to `src/core/settings.cpp` immediately after `enableBLEAPI()` (after line 1684):

```cpp
bool bleApiIsEnabled() { return ble_api_enabled; }
```

c. Create `src/core/serial_commands/attack_commands.h`:

```cpp
#pragma once
#if !defined(LITE_VERSION)
#include <Arduino.h>
class SimpleCLI;
void createAttackCommands(SimpleCLI *cli);
#endif
```

d. Create `src/core/serial_commands/attack_commands.cpp`:

```cpp
#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/settings.h"
#include <SimpleCLI.h>
#include <globals.h>

uint32_t bleApiCmdCallback(cmd *c) {
    Command cmd(c);
    String state = cmd.getArgument("state").getValue();
    state.trim();
    if (state == "on") {
        if (!bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return bleApiIsEnabled();
    } else if (state == "off") {
        if (bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return !bleApiIsEnabled();
    }
    serialDevice->println("usage: ble api on|off");
    return false;
}

void createAttackCommands(SimpleCLI *cli) {
    Command ble = cli->addCompositeCmd("ble");
    Command bleApi = ble.addCommand("api", bleApiCmdCallback);
    bleApi.addPosArg("state", "on");
}
#endif
```

e. Register in `src/core/serial_commands/cli.cpp`. At the top, add to the existing include block (after line 15 `#include "util_commands.h"`):

```cpp
#include "attack_commands.h"
```

Inside `SerialCli::setup()` (line 34), inside the `#if !defined(LITE_VERSION)` block at line 52 where `createInterpreterCommands(&_cli);` lives, add:

```cpp
    createInterpreterCommands(&_cli);
    createAttackCommands(&_cli);   // <-- add this line
```

- [ ] **Step 4: Run test to verify it passes**

Build + flash:
```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
```

Then:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ble%20api%20on'
```
Expected: `200` with body `command ble api on queued`. On the device serial log you should see `BLE API ON` and the BLE advertising name `Bruc`. Verify by scanning for BLE devices from your phone — `Bruc` should appear.

Test the off path:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ble%20api%20off'
```
Expected: `200`, serial log `BLE API OFF`, `Bruc` disappears from BLE scans.

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.h src/core/serial_commands/attack_commands.cpp src/core/serial_commands/cli.cpp src/core/settings.h src/core/settings.cpp
git commit -m "feat(serial): add 'ble api on|off' verb + attack_commands scaffold"
```

---

## Task 2: `evilportal <ssid> <ch> [template]` verb (Mission 1 enabler)

Mission 1 (Evil Portal Campaign) hinges on this single verb. The constructor already takes parameters and runs autonomously — no TFT steering needed, the cleanest one-tap in the patch.

**Files:**
- Modify: `src/core/serial_commands/attack_commands.cpp` (add callback + register verb)
- Modify: `src/core/serial_commands/attack_commands.cpp` (`createAttackCommands` body)

**Interfaces:**
- Consumes: `EvilPortal(String tssid="", uint8_t channel=6, bool deauth=false, bool verifyPwd=false, bool autoMode=false, bool backgroundMode=false, String templateFile="")` from `modules/wifi/evil_portal.h`. The on-device menu calls it as `EvilPortal();` (`WifiMenu.cpp:65`) — we just pass args through.
- Produces: verb `evilportal <ssid> <ch> [template]` callable over `/cm`.

- [ ] **Step 1: Write the failing test**

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=evilportal%20FreeWifi%206'
```
Expected PRE: `400` / unknown command.

- [ ] **Step 2: Run test to verify it fails**

Run the curl above. Expected: `400` and an Unknown-command error in the serial log.

- [ ] **Step 3: Write minimal implementation**

a. At the top of `src/core/serial_commands/attack_commands.cpp`, add the include (after `#include "core/settings.h"`):

```cpp
#include "modules/wifi/evil_portal.h"
```

b. Add the callback (above `createAttackCommands`):

```cpp
uint32_t evilportalCmdCallback(cmd *c) {
    Command cmd(c);
    String ssid = cmd.getArgument("ssid").getValue();
    String chStr = cmd.getArgument("channel").getValue();
    String templateFile = cmd.getArgument("template").getValue();
    ssid.trim();
    chStr.trim();
    templateFile.trim();
    if (ssid.isEmpty()) ssid = "Free Wifi";
    uint8_t channel = (uint8_t)chStr.toInt();
    if (channel < 1 || channel > 13) channel = 6;
    // autoMode=true so the constructor does NOT loop menu options on TFT
    EvilPortal(ssid, channel, false, false, true, false, templateFile);
    return true;
}
```

c. In `createAttackCommands`, append (after the `ble` composite cmd):

```cpp
    Command evilportal = cli->addCommand("evilportal", evilportalCmdCallback);
    evilportal.addPosArg("ssid", "Free Wifi");
    evilportal.addPosArg("channel", "6");
    evilportal.addPosArg("template", "");
```

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=evilportal%20FreeWifi%206'
```
Expected: `200`, body `command evilportal FreeWifi 6 queued`. On the device: TFT shows "EVIL PORTAL / Starting…", then Bruce broadcasts SSID `FreeWifi` on channel 6. Verify from a second phone: `FreeWifi` appears in Wi-Fi networks; joining it routes to the captive portal.

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.cpp
git commit -m "feat(serial): add 'evilportal <ssid> <ch> [template]' verb"
```

---

## Task 3: `blespam <type> <count>` verb (Mission 2 enabler)

Mission 2 (BLE Chaos). The FastPair spam primitive is fully headless (`FastPairExploitEngine::spamFastPairPopups(type, count)`), no target needed — it broadcasts popup advertisements. Other spam types route through `spamMenu()` which opens the TFT; we expose those via `blespam menu` for completeness.

**Files:**
- Modify: `src/core/serial_commands/attack_commands.cpp`

**Interfaces:**
- Consumes: `FastPairExploitEngine::spamFastPairPopups(FastPairPopupType popupType, int count)` (`modules/ble/BLE_Suite.h:261`). Enum `FastPairPopupType { FP_POPUP_REGULAR=0, FP_POPUP_FUN, FP_POPUP_PRANK, FP_POPUP_CUSTOM }` (`BLE_Suite.h:41`). Also `void spamMenu();` from `modules/ble/ble_spam.h:8` (TFT-driven fallback).
- Produces: verb `blespam <type> <count>` where `type` ∈ `{fastpair_regular, fastpair_fun, fastpair_prank, fastpair_custom, menu}`.

- [ ] **Step 1: Write the failing test**

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20fastpair_regular%2010'
```
Expected PRE: `400` / unknown.

- [ ] **Step 2: Run test to verify it fails**

Run curl. Expected: `400`, Unknown-command in serial log.

- [ ] **Step 3: Write minimal implementation**

a. Add includes at the top of `attack_commands.cpp`:

```cpp
#include "modules/ble/BLE_Suite.h"
#include "modules/ble/ble_spam.h"
```

b. Add the callback:

```cpp
uint32_t blespamCmdCallback(cmd *c) {
    Command cmd(c);
    String typeStr = cmd.getArgument("type").getValue();
    String countStr = cmd.getArgument("count").getValue();
    typeStr.trim();
    countStr.trim();
    int count = countStr.toInt();
    if (count < 1) count = 10;

    FastPairPopupType fpType;
    bool useFastPair = true;
    if (typeStr == "fastpair_regular")      fpType = FP_POPUP_REGULAR;
    else if (typeStr == "fastpair_fun")     fpType = FP_POPUP_FUN;
    else if (typeStr == "fastpair_prank")   fpType = FP_POPUP_PRANK;
    else if (typeStr == "fastpair_custom")  fpType = FP_POPUP_CUSTOM;
    else { useFastPair = false; }

    if (useFastPair) {
        FastPairExploitEngine fpEngine;
        fpEngine.spamFastPairPopups(fpType, count);
        return true;
    }
    if (typeStr == "menu") {
        spamMenu();   // TFT-driven — user steers on the device
        return true;
    }
    serialDevice->println("usage: blespam <fastpair_regular|fastpair_fun|fastpair_prank|fastpair_custom|menu> <count>");
    return false;
}
```

c. In `createAttackCommands`:

```cpp
    Command blespam = cli->addCommand("blespam", blespamCmdCallback);
    blespam.addPosArg("type", "fastpair_regular");
    blespam.addPosArg("count", "10");
```

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20fastpair_regular%2010'
```
Expected: `200`, device spams 10 FastPair popups. Verify from a nearby Android phone — you should see a flurry of "Fast Pair" pairing prompts. The `Bruc` BLE name should briefly appear during the cycles.

Also smoke the menu fallback:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20menu'
```
Expected: `200`, device opens the BLE spam menu on the TFT (user selects the spam type on the device).

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.cpp
git commit -m "feat(serial): add 'blespam <type> <count>' verb (FastPair headless + menu fallback)"
```

---

## Task 4: `karma`, `deauth`, `blesniffer` menu-dispatcher verbs

These three reach features whose menu-dispatchers need TFT steering today (the menu reads parameters from the device). They're still useful — one tap launches the corresponding menu instead of phone-side drilling — and they're explicitly labeled menu-dispatcher in the API doc. A future patch can refactor the underlying attacks to be headless; this plan does not.

**Files:**
- Modify: `src/core/serial_commands/attack_commands.cpp`

**Interfaces:**
- Consumes: `void karma_setup();` (`modules/wifi/karma_attack.h` — confirmed by WifiMenu `#include` + line `void karma_setup();`). `void wifi_atk_menu();` (`modules/wifi/wifi_atks.h:46`). `void BleSuiteMenu();` (`modules/ble/BLE_Suite.h` — called as `BleSuiteMenu();` at `BleMenu.cpp:39`).
- Produces: verbs `karma`, `deauth [<target>]`, `blesniffer` (each opens its menu on TFT; `deauth`'s optional target arg is currently ignored — pending a future patch).

- [ ] **Step 1: Write the failing test**

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=karma'
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=deauth'
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blesniffer'
```
Expected PRE: all three return `400` / unknown.

- [ ] **Step 2: Run test to verify it fails**

Run all three curls. Expected: `400` for each.

- [ ] **Step 3: Write minimal implementation**

a. Add includes at the top of `attack_commands.cpp`:

```cpp
#include "modules/wifi/karma_attack.h"
#include "modules/wifi/wifi_atks.h"
#include "modules/ble/BLE_Suite.h"
```

b. Add the three callbacks (they all delegate to the existing TFT-driven entries):

```cpp
uint32_t karmaCmdCallback(cmd *c) {
    karma_setup();
    return true;
}

uint32_t deauthCmdCallback(cmd *c) {
    // wifi_atk_menu opens the deauth submenu on the TFT; the optional target arg
    // is accepted for API symmetry but the user still confirms on the device.
    wifi_atk_menu();
    return true;
}

uint32_t blesnifferCmdCallback(cmd *c) {
    BleSuiteMenu();
    return true;
}
```

c. In `createAttackCommands`:

```cpp
    cli->addCommand("karma", karmaCmdCallback);
    Command deauth = cli->addCommand("deauth", deauthCmdCallback);
    deauth.addPosArg("target", "");
    cli->addCommand("blesniffer", blesnifferCmdCallback);
```

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=karma'
```
Expected: `200`, device opens the Karma configuration menu on the TFT.

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=deauth'
```
Expected: `200`, device opens the Wi-Fi Atks menu.

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blesniffer'
```
Expected: `200`, device opens the BLE Suite menu.

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.cpp
git commit -m "feat(serial): add karma, deauth, blesniffer menu-dispatcher verbs"
```

---

## Task 5: `ap_info`, `reverseshell`, `pwngrid` one-tap verbs

Three more one-tap wrappers for the Manual Console. `ap_info` shows current AP info on the TFT (Mission 3 stage 2). `reverseshell` and `pwngrid` start ReverseShell and pwnagotchi (Brucegotchi) respectively; both run autonomously once started.

**Files:**
- Modify: `src/core/serial_commands/attack_commands.cpp`

**Interfaces:**
- Consumes: `void displayAPInfo();` (`modules/wifi/ap_info.h:4`, WifiMenu calls it at `WifiMenu.cpp:60`). `void ReverseShell();` (`modules/reverseShell/reverseShell.h:7`). `void brucegotchi_start();` (`modules/pwnagotchi/pwnagotchi.h:10`, WifiMenu calls it at `WifiMenu.cpp:94`).
- Produces: verbs `ap_info`, `reverseshell`, `pwngrid`.

- [ ] **Step 1: Write the failing test**

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ap_info'
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=reverseshell'
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=pwngrid'
```
Expected PRE: all `400` / unknown.

- [ ] **Step 2: Run test to verify it fails**

Run all three. Expected: `400` for each.

- [ ] **Step 3: Write minimal implementation**

a. Add includes:

```cpp
#include "modules/wifi/ap_info.h"
#include "modules/reverseShell/reverseShell.h"
#include "modules/pwnagotchi/pwnagotchi.h"
```

b. Add callbacks:

```cpp
uint32_t apInfoCmdCallback(cmd *c) {
    displayAPInfo();
    return true;
}

uint32_t reverseshellCmdCallback(cmd *c) {
    ReverseShell();
    return true;
}

uint32_t pwngridCmdCallback(cmd *c) {
    brucegotchi_start();
    return true;
}
```

c. In `createAttackCommands`:

```cpp
    cli->addCommand("ap_info", apInfoCmdCallback);
    cli->addCommand("reverseshell", reverseshellCmdCallback);
    cli->addCommand("pwngrid", pwngridCmdCallback);
```

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=ap_info'
```
Expected: `200`, device displays AP info on the TFT (requires Bruce to be in STA mode — connect to a known Wi-Fi first via `wifi on`).

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=pwngrid'
```
Expected: `200`, Brucegotchi face appears on TFT and starts pwngrid.

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=reverseshell'
```
Expected: `200`, device enters reverse-shell listening mode (visible on serial log; needs network connectivity to be useful).

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.cpp
git commit -m "feat(serial): add ap_info, reverseshell, pwngrid one-tap verbs"
```

---

## Task 6: Bearer-token auth in `checkUserWebAuth`

Lets the app send `Authorization: Bearer <token>` instead of juggling cookies (RN's `fetch` makes cookie-jars awkward). `~3` lines added before the cookie check.

**Files:**
- Modify: `src/core/wifi/webInterface.cpp:171` (`checkUserWebAuth`)

**Interfaces:**
- Consumes: `bool BruceConfig::isValidWebUISession(const String &token)` (`config.cpp:871`) — same validator the cookie path uses.
- Produces: `checkUserWebAuth` accepts `Authorization: Bearer <token>` OR the existing cookie.

- [ ] **Step 1: Write the failing test**

```bash
curl -H "Authorization: Bearer <token>" 'http://bruce.local/systeminfo'
```
Expected PRE: `401 Unauthorized` (bearer not yet parsed).

- [ ] **Step 2: Run test to verify it fails**

Run the curl. Expected: `401`, body `Unauthorized`.

- [ ] **Step 3: Write minimal implementation**

In `src/core/wifi/webInterface.cpp`, modify `checkUserWebAuth` at line 171. Replace the existing body opening:

```cpp
bool checkUserWebAuth(AsyncWebServerRequest *request, bool onFailureReturnLoginPage = false) {
    if (request->hasHeader("Cookie")) {
```

with:

```cpp
bool checkUserWebAuth(AsyncWebServerRequest *request, bool onFailureReturnLoginPage = false) {
    // Authorization: Bearer <token> (mobile companion convenience)
    if (request->hasHeader("Authorization")) {
        const AsyncWebHeader *auth = request->getHeader("Authorization");
        String v = auth->value();
        const char *prefix = "Bearer ";
        if (v.startsWith(prefix)) {
            String token = v.substring(strlen(prefix));
            token.trim();
            if (bruceConfig.isValidWebUISession(token)) return true;
        }
    }
    if (request->hasHeader("Cookie")) {
```

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -H "Authorization: Bearer <token>" 'http://bruce.local/systeminfo'
```
Expected: `200` with the (still pre-extension) systeminfo JSON. The cookie path still works:

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/systeminfo'
```
Expected: `200` (no regression).

- [ ] **Step 5: Commit**

```bash
git add src/core/wifi/webInterface.cpp
git commit -m "feat(web): accept Bearer token in checkUserWebAuth (mobile companion)"
```

---

## Task 7: `/ws` AsyncWebSocket server + `pushWsEvent` + `lastEventId`

The companion app's live event feed. ESPAsyncWebServer bundles `AsyncWebSocket`. We register `/ws`, push JSON event frames with monotonic ids, and accept `{"cmd":"subscribe","since":N}` from a client to resume from N+1.

**Files:**
- Create: `src/core/wifi/ws_events.h`
- Create: `src/core/wifi/ws_events.cpp`
- Modify: `src/core/wifi/webInterface.cpp:396` (`configureWebServer`) — call `beginWsServer(server);` immediately before `server->begin();` (line 740).

**Interfaces:**
- Consumes: `AsyncWebServer *server` (the existing global in `webInterface.cpp`). ESPAsyncWebServer's `AsyncWebSocket`/`AsyncWebSocketClient`.
- Produces (called by later tasks): `pushWsEvent(type, jsonPayload)`, `pushWsLog(line, level)`, `setDeviceState(state)`, `getDeviceState()`.

- [ ] **Step 1: Write the failing test**

```bash
# requires wscat: npm i -g wscat
wscat -c 'ws://bruce.local/ws'
```
Expected PRE: connection refused (no `/ws` route yet).

- [ ] **Step 2: Run test to verify it fails**

Run wscat. Expected: error / connection refused.

- [ ] **Step 3: Write minimal implementation**

a. Create `src/core/wifi/ws_events.h`:

```cpp
#pragma once
#if !defined(LITE_VERSION)
#include <Arduino.h>
class AsyncWebServer;
void beginWsServer(AsyncWebServer *server);
void pushWsEvent(const String &type, const String &jsonPayload);
void pushWsLog(const String &line, const char *level = "info");
void setDeviceState(const String &state);
String getDeviceState();
#endif
```

b. Create `src/core/wifi/ws_events.cpp`:

```cpp
#if !defined(LITE_VERSION)
#include "ws_events.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebSocket ws("/ws");
static uint32_t wsEventId = 0;
static String deviceState = "idle";

void beginWsServer(AsyncWebServer *server) {
    ws.onEvent([](AsyncWebSocket *srv, AsyncWebSocketClient *cli, AwsEventType type,
                 void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            cli->text(String("{\"id\":") + wsEventId + ",\"type\":\"state\",\"device_state\":\"" + deviceState + "\"}");
        }
        // Data from client (e.g. {"cmd":"subscribe","since":N}) is accepted but
        // currently ignored — clients just receive events pushed after connect.
        // lastEventId resume is implemented app-side by re-fetching /systeminfo.
    });
    server->addHandler(&ws);
}

void pushWsEvent(const String &type, const String &jsonPayload) {
    if (ws.count() == 0) return;  // avoid cost when nobody listens
    String frame = "{\"id\":" + String(++wsEventId) + ",\"type\":\"" + type + "\"" + jsonPayload + "}";
    ws.textAll(frame);
}

void pushWsLog(const String &line, const char *level) {
    String escaped = line;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    pushWsEvent("log", ",\"line\":\"" + escaped + "\",\"level\":\"" + level + "\"");
}

void setDeviceState(const String &state) {
    deviceState = state;
    pushWsEvent("state", ",\"device_state\":\"" + state + "\"");
}

String getDeviceState() { return deviceState; }
#endif
```

c. Modify `src/core/wifi/webInterface.cpp`:

Add near other includes (after `#include <ESPAsyncWebServer.h>` line):

```cpp
#include "core/wifi/ws_events.h"
```

In `configureWebServer()` (line 396), immediately before `server->begin();` at line 740, add:

```cpp
    beginWsServer(server);
    server->begin();
```

(Replace the existing `server->begin();` line — don't add a second call.)

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
wscat -c 'ws://bruce.local/ws'
```
Expected: connection accepted, immediate frame `{"id":0,"type":"state","device_state":"idle"}`. Type any message and the connection stays open.

- [ ] **Step 5: Commit**

```bash
git add src/core/wifi/ws_events.h src/core/wifi/ws_events.cpp src/core/wifi/webInterface.cpp
git commit -m "feat(web): add /ws AsyncWebSocket event stream + pushWsEvent helper"
```

---

## Task 8: Hook `showAttackProgress`/`showAttackResult` → `/ws`

For Mission 2 (BLE Chaos). Each call to the two UI helpers now also pushes a `/ws` frame, so the app sees "Spammed N popups" as it happens without polling `/getscreen`. Two lines added, two functions.

**Files:**
- Modify: `src/modules/ble/BLE_Suite.cpp:5933` (`showAttackProgress`) + `:5985` (`showAttackResult`)

**Interfaces:**
- Consumes: `pushWsEvent(type, jsonPayload)` from `ws_events.h`.
- Produces: `/ws` frames `{type:'ble_progress', msg, color?}` and `{type:'ble_result', success, msg}`.

- [ ] **Step 1: Write the failing test**

With `wscat -c 'ws://bruce.local/ws'` open in one terminal, run:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20fastpair_regular%205'
```
Expected PRE: spam runs but no `ble_progress` frames reach wscat (only the initial `state` frame).

- [ ] **Step 2: Run test to verify it fails**

Run the curl with wscat connected. Expected: wscat stays silent (apart from the connect frame) while the device spams.

- [ ] **Step 3: Write minimal implementation**

In `src/modules/ble/BLE_Suite.cpp`, near the top of the file's includes (after the existing NimBLE includes), within the `#if !defined(LITE_VERSION)` block at the top of the file if one exists, add:

```cpp
#if !defined(LITE_VERSION)
#include "core/wifi/ws_events.h"
#endif
```

(If the file uses a single top-level guard rather than per-include guards, place it under the same guard — verify by searching for existing `#include "core/` lines in this file and matching the pattern.)

Modify `showAttackProgress` at line 5933. After the function's existing `tft.fillScreen(bruceConfig.bgColor);` line (line 5934), add:

```cpp
    pushWsEvent("ble_progress", String(",\"msg\":\"") + message + "\"");
```

Modify `showAttackResult` at line 5985. After the function opening line `if (success) {`, before whatever the existing first statement is, add:

```cpp
    pushWsEvent("ble_result", String(",\"success\":") + (success ? "true" : "false") + ",\"msg\":\"" + message + "\"");
```

(If `showAttackResult`'s body differs from the snippet seen — the explore output was truncated at line 5985 — read the surrounding 10 lines before editing to place the `pushWsEvent` call at the top of the function body, after the variable setup.)

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
# in one terminal:
wscat -c 'ws://bruce.local/ws'
# in another:
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20fastpair_regular%205'
```
Expected: wscat receives multiple `{"id":N,"type":"ble_progress","msg":"Starting…"}` (or similar) frames during the spam and one `{"id":N,"type":"ble_result","success":true,"msg":"…"}` at the end.

- [ ] **Step 5: Commit**

```bash
git add src/modules/ble/BLE_Suite.cpp
git commit -m "feat(ble): push showAttackProgress/showAttackResult to /ws for live telemetry"
```

---

## Task 9: Hook CLI output → `/ws` `log` + `state` frames

Forwards CLI serial output to `/ws` (so the app sees command responses) and pushes `state` frames when attacks start/stop. The hook is small and central: every `/cm` command response becomes a `log` frame; specific attack verb callbacks set `state` via `setDeviceState(...)`.

**Files:**
- Modify: `src/core/serialcmds.cpp:37` (`handleSerialCommands`)
- Modify: `src/core/serial_commands/attack_commands.cpp` (Task 2 & 3 callbacks — wrap with `setDeviceState` calls)

**Interfaces:**
- Consumes: `pushWsLog(line, level)` and `setDeviceState(state)` from `ws_events.h`. The existing `serialDevice->println(...)` calls inside callbacks already write the line — we capture them by forwarding in `handleSerialCommands`.
- Produces: `/ws` `log` frames for every CLI response, and `state` frames when attacks start/stop.

- [ ] **Step 1: Write the failing test**

With `wscat -c 'ws://bruce.local/ws'` open, run a benign command:
```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=info'
```
Expected PRE: wscat receives only the initial `state` frame; the device's `info` response goes only to USB serial.

- [ ] **Step 2: Run test to verify it fails**

Run the curl with wscat connected. Expected: wscat silent after the connect frame.

- [ ] **Step 3: Write minimal implementation**

a. In `src/core/serialcmds.cpp`, near the top includes, add (guarded):

```cpp
#if !defined(LITE_VERSION)
#include "core/wifi/ws_events.h"
#endif
```

b. In `handleSerialCommands` (line 37), after `serialCli.parse(String(packet.text));` (the queued path at line 41) and after the live-serial `serialCli.parse(cmd_str);` (line 51), forward the command text and the result line to `/ws`. Modify the queued path:

Replace (lines 40-46, the existing `if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) { ... }` block):
```cpp
        if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) {
            bool result = serialCli.parse(String(packet.text));
            xQueueSend(rspQueue, &result, 0);
            Serial.println("COMMAND: " + String(packet.text));
            Serial.printf("[CLI] Result: %s\n", result ? "TRUE" : "FALSE");
        }
```

with:

```cpp
        if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) {
#if !defined(LITE_VERSION)
            pushWsLog(String("COMMAND: ") + packet.text, "info");
#endif
            bool result = serialCli.parse(String(packet.text));
            xQueueSend(rspQueue, &result, 0);
            Serial.println("COMMAND: " + String(packet.text));
            Serial.printf("[CLI] Result: %s\n", result ? "TRUE" : "FALSE");
#if !defined(LITE_VERSION)
            pushWsLog(String("[CLI] Result: ") + (result ? "TRUE" : "FALSE"), "info");
#endif
        }
```

c. In `attack_commands.cpp`, add `setDeviceState` calls to mark attack start/stop. Add the include next to the others:

```cpp
#include "core/wifi/ws_events.h"
```

In `evilportalCmdCallback`, before the `EvilPortal(...)` call:

```cpp
    setDeviceState("portal");
    EvilPortal(ssid, channel, false, false, true, false, templateFile);
    setDeviceState("idle");
```

In `blespamCmdCallback` (fastpair branches), before and after `fpEngine.spamFastPairPopups(...)`:

```cpp
        setDeviceState("ble_spam");
        FastPairExploitEngine fpEngine;
        fpEngine.spamFastPairPopups(fpType, count);
        setDeviceState("idle");
```

(Skip `state` frames for the menu-dispatcher verbs in Task 4 — they return immediately after opening the menu; state would go "menu" → "idle" within a tick and isn't useful. Only the long-running autonomous verbs get `state`.)

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
# wscat open
wscat -c 'ws://bruce.local/ws'
# in another terminal
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=info'
```
Expected: wscat receives `{"id":N,"type":"log","line":"COMMAND: info","level":"info"}` followed by `{"id":N+1,"type":"log","line":"[CLI] Result: TRUE","level":"info"}`.

```bash
curl -b "BRUCESESSION=<token>" 'http://bruce.local/cm?cmnd=blespam%20fastpair_regular%203'
```
Expected: wscat receives `{"type":"state","device_state":"ble_spam"}`, then `ble_progress`×N, `ble_result`, `{"type":"state","device_state":"idle"}`.

- [ ] **Step 5: Commit**

```bash
git add src/core/serialcmds.cpp src/core/serial_commands/attack_commands.cpp
git commit -m "feat(web): forward CLI output to /ws + push state frames for attacks"
```

---

## Task 10: Extend `/systeminfo` with capabilities + telemetry

The companion app pulls this once at startup to grey out features absent on the connected board. Capability flags are read from compile-time `#define`s.

**Files:**
- Modify: `src/core/wifi/webInterface.cpp:461` (`/systeminfo` route)

**Interfaces:**
- Consumes: `getBattery()` & `isCharging()` (defined per-board in `boards/smoochiee-board/interface.cpp`). `WiFi.getMode()`, `WiFi.localIP()`, `ESP.getFreeHeap()`, `psramFound()`. Compile-time flags: `USB_as_HID`, `HAS_SCREEN`, `LITE_VERSION`, `USE_CC1101_VIA_SPI`, `USE_NRF24_VIA_SPI`, `has_pn532` (Bruce does NOT currently #define this — we treat PN532 as absent unless a future board flag exists; spec confirms smoochiee has no PN532), `HAS_GPS_SERIAL`/`GPS_SERIAL_TX` (smoochiee defines `GPS_SERIAL_TX`/`GPS_SERIAL_RX`), `IR_TX_PINS` (build-flag defines list on smoochiee), `HAS_FM` (no `#define` — set false), `HAS_ETH_PHY` (no `#define` — set false), `BUZZ_PIN`, `HAS_RGB_LED`, `MIC_INMP441`.
- Produces: extended `/systeminfo` JSON with the `capabilities` object + telemetry fields per spec §5.4.

- [ ] **Step 1: Write the failing test**

```bash
curl -H "Authorization: Bearer <token>" 'http://bruce.local/systeminfo'
```
Expected PRE: JSON with only `BRUCE_VERSION`, `SD`, `LittleFS` (the existing body at `webInterface.cpp:472-486`).

- [ ] **Step 2: Run test to verify it fails**

Run the curl. Expected: response body lacks `capabilities`, `battery_pct`, etc.

- [ ] **Step 3: Write minimal implementation**

In `src/core/wifi/webInterface.cpp`, replace the `/systeminfo` handler body (lines 461-490) with an extended JSON. Use `ArduinoJson` (already a Bruce dependency — `ArduinoJson.h`) to build the response, which avoids manual JSON-escaping bugs.

Replace the existing handler:

```cpp
    server->on("/systeminfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            char response_body[300];
            uint64_t LittleFSTotalBytes = LittleFS.totalBytes();
            uint64_t LittleFSUsedBytes = LittleFS.usedBytes();
            uint64_t SDTotalBytes = SD.totalBytes();
            uint64_t SDUsedBytes = SD.usedBytes();
            snprintf(
                response_body, sizeof(response_body),
                "{\"%s\":\"%s\",\"SD\":{...},\"LittleFS\":{...}}",
                "BRUCE_VERSION", BRUCE_VERSION /* ... */
            );
            request->send(200, "application/json", response_body);
        }
    });
```

(Keep the existing SD/LittleFS block — `webInterface.cpp:463-487` — as-is; only extend after it.)

with:

```cpp
    server->on("/systeminfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!checkUserWebAuth(request)) return;
        JsonDocument doc;
        doc["BRUCE_VERSION"] = BRUCE_VERSION;
        JsonObject sd = doc["SD"].to<JsonObject>();
        sd["free"] = humanReadableSize(SD.totalBytes() - SD.usedBytes());
        sd["used"] = humanReadableSize(SD.usedBytes());
        sd["total"] = humanReadableSize(SD.totalBytes());
        JsonObject lfs = doc["LittleFS"].to<JsonObject>();
        lfs["free"] = humanReadableSize(LittleFS.totalBytes() - LittleFS.usedBytes());
        lfs["used"] = humanReadableSize(LittleFS.usedBytes());
        lfs["total"] = humanReadableSize(LittleFS.totalBytes());

        JsonObject caps = doc["capabilities"].to<JsonObject>();
        caps["usb_as_hid"]    = (bool)(defined(USB_as_HID) && USB_as_HID);
        caps["has_screen"]    = (bool)defined(HAS_SCREEN);
        caps["lite_version"]  = (bool)defined(LITE_VERSION);
        caps["has_cc1101"]   = (bool)defined(USE_CC1101_VIA_SPI);
        caps["has_nrf24"]    = (bool)defined(USE_NRF24_VIA_SPI);
        caps["has_pn532"]    = false;  // Bruce has no PN532 build flag; smoochiee absence confirmed
        caps["has_gps"]      = (bool)defined(GPS_SERIAL_TX);
        caps["has_ir"]       = (bool)defined(IR_TX_PINS);
        caps["has_fm"]       = false;  // no FM build flag in this firmware
        caps["has_eth"]      = false;  // no ethernet build flag for smoochiee
        caps["has_buzz"]     = (bool)defined(BUZZ_PIN);
        caps["has_rgb_led"]  = (bool)defined(HAS_RGB_LED);
        caps["has_mic"]      = (bool)defined(MIC_INMP441);

        doc["battery_pct"] = getBattery();
        doc["charging"]    = isCharging();
        doc["wifi_mode"]   = (int)WiFi.getMode();
        doc["ip"]          = WiFi.localIP().toString();
        doc["free_heap"]   = (int)ESP.getFreeHeap();
        doc["psram"]       = psramFound();

        String body;
        serializeJson(doc, body);
        request->send(200, "application/json", body);
    });
```

Note on `defined()`: in Arduino-ESP32 preprocessor, `defined(X)` evaluates as `1`/`0` directly when used in C++ code as `#if defined(X)`. For an inline boolean expression outside `#if`, you need a helper — either wrap each `(bool)defined(X)` use in a `#if/#else` block to fold the literal true/false, OR define helper macros at the top of the route handler. Use this simpler verified pattern instead of the inline `defined()` shown above:

```cpp
        // Build capability literals at compile time — avoids runtime defined() (which isn't valid C++)
        #define BS_HAS(flag, name) (bool)(name)
        JsonObject caps = doc["capabilities"].to<JsonObject>();
    #if defined(USB_as_HID) && USB_as_HID
        caps["usb_as_hid"] = true;
    #else
        caps["usb_as_hid"] = false;
    #endif
    #if defined(HAS_SCREEN)
        caps["has_screen"] = true;
    #else
        caps["has_screen"] = false;
    #endif
    #if defined(LITE_VERSION)
        caps["lite_version"] = true;
    #else
        caps["lite_version"] = false;
    #endif
    #if defined(USE_CC1101_VIA_SPI)
        caps["has_cc1101"] = true;
    #else
        caps["has_cc1101"] = false;
    #endif
    #if defined(USE_NRF24_VIA_SPI)
        caps["has_nrf24"] = true;
    #else
        caps["has_nrf24"] = false;
    #endif
        caps["has_pn532"] = false;
    #if defined(GPS_SERIAL_TX)
        caps["has_gps"] = true;
    #else
        caps["has_gps"] = false;
    #endif
    #if defined(IR_TX_PINS)
        caps["has_ir"] = true;
    #else
        caps["has_ir"] = false;
    #endif
        caps["has_fm"] = false;
        caps["has_eth"] = false;
    #if defined(BUZZ_PIN)
        caps["has_buzz"] = true;
    #else
        caps["has_buzz"] = false;
    #endif
    #if defined(HAS_RGB_LED)
        caps["has_rgb_led"] = true;
    #else
        caps["has_rgb_led"] = false;
    #endif
    #if defined(MIC_INMP441)
        caps["has_mic"] = true;
    #else
        caps["has_mic"] = false;
    #endif
```

(Use the `#if/#else/#endif` form shown above; drop the inline `defined()` form. The `ArduinoJson` `JsonDocument` import and `SD`/`LittleFS` reads stay identical.)

- [ ] **Step 4: Run test to verify it passes**

```bash
pio run -e smoochiee-board && pio run -e smoochiee-board -t upload
curl -H "Authorization: Bearer <token>" 'http://bruce.local/systeminfo'
```
Expected: 200 with JSON containing `capabilities` (matching smoochiee: `usb_as_hid:true, has_screen:true, lite_version:false, has_cc1101:false, has_nrf24:false, has_pn532:false, has_gps:false, has_ir:true, has_fm:false, has_eth:false, has_buzz:true, has_rgb_led:true, has_mic:true`), `battery_pct` (integer 0-100), `charging` (boolean), `wifi_mode` (0-3 integer), `ip` (string), `free_heap` (integer), `psram` (true on smoochiee).

Validate the JSON parses cleanly:
```bash
curl -s -H "Authorization: Bearer <token>" 'http://bruce.local/systeminfo' | python3 -m json.tool
```
Expected: pretty-printed JSON with no parse errors.

- [ ] **Step 5: Commit**

```bash
git add src/core/wifi/webInterface.cpp
git commit -m "feat(web): extend /systeminfo with capabilities + battery + network telemetry"
```

---

## Task 11: Smoke-test script + API contract doc

Drop a runnable test artifact and freeze the contract the companion app will vendor. The script doubles as the demo-day smoke check.

**Files:**
- Create: `tools/smoke_test_attacks.sh`
- Create: `docs/bruce-companion-api.md`

**Interfaces:**
- Consumes: all verbs and endpoints shipped by Tasks 1-10.
- Produces: a one-command regression check + the contract document the app repo imports.

- [ ] **Step 1: Write the failing test (trivially — script doesn't exist)**

```bash
bash tools/smoke_test_attacks.sh
```
Expected PRE: `bash: tools/smoke_test_attacks.sh: No such file or directory`.

- [ ] **Step 2: Run test to verify it fails**

Run the bash command. Expected: file-not-found error.

- [ ] **Step 3: Write minimal implementation**

a. Create `tools/smoke_test_attacks.sh`:

```bash
#!/usr/bin/env bash
# Bruce firmware patch smoke test. Run against a flashed smoochiee-board on the
# same Wi-Fi as your workstation (or its AP). Set BRUCE_URL + BRUCE_TOKEN.
# Usage: BRUCE_URL=http://bruce.local BRUCE_TOKEN=<token> bash tools/smoke_test_attacks.sh
set -euo pipefail
: "${BRUCE_URL:?need BRUCE_URL}"
: "${BRUCE_TOKEN:?need BRUCE_TOKEN}"

auth="Authorization: Bearer ${BRUCE_TOKEN}"
pass=0; fail=0
chk() {
    local name="$1"; local url="$2"; local want="${3:-200}"
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$auth" "$url")
    if [ "$code" = "$want" ]; then
        echo "PASS  $name -> $code"; pass=$((pass+1))
    else
        echo "FAIL  $name -> got $code want $want"; fail=$((fail+1))
    fi
}

echo "== Existing endpoints (regression) =="
chk "GET  /systeminfo"    "${BRUCE_URL}/systeminfo"
chk "GET  /getscreen"     "${BRUCE_URL}/getscreen"
chk "GET  /listfiles"     "${BRUCE_URL}/listfiles?fs=LittleFS"

echo "== New attack verbs (HTTP 200 = queued) =="
chk "POST ble api on"     "${BRUCE_URL}/cm?cmnd=ble%20api%20on"
chk "POST evilportal"     "${BRUCE_URL}/cm?cmnd=evilportal%20FreeWifi%206"
sleep 3   # let evil portal spin up — comment out if you don't have a victim device ready
chk "POST blespam fastpair" "${BRUCE_URL}/cm?cmnd=blespam%20fastpair_regular%203"
chk "POST karma"          "${BRUCE_URL}/cm?cmnd=karma"
chk "POST deauth"         "${BRUCE_URL}/cm?cmnd=deauth"
chk "POST blesniffer"     "${BRUCE_URL}/cm?cmnd=blesniffer"
chk "POST ap_info"        "${BRUCE_URL}/cm?cmnd=ap_info"
chk "POST reverseshell"   "${BRUCE_URL}/cm?cmnd=reverseshell"
chk "POST pwngrid"        "${BRUCE_URL}/cm?cmnd=pwngrid"
chk "POST ble api off"    "${BRUCE_URL}/cm?cmnd=ble%20api%20off"

echo "== Bearer auth =="
chk "Bearer 401 wrong"    "${BRUCE_URL}/systeminfo" "401"
# re-test with no header to confirm 401
chk "No auth 401"         "${BRUCE_URL}/systeminfo" "401"

echo "== Summary =="
echo "PASS=$pass  FAIL=$fail"
exit $((fail > 0))
```

(For the 401 negative tests, drop the `-H "$auth"` using a separate `curl` — the smoke above is illustrative; the test for `Bearer wrong` is left out of `chk` because `chk` always sends the bearer header. Replace the last two `chk` calls with raw curls:

```bash
echo "== Negative auth =="
curl -s -o /dev/null -w 'no-authandle -> %{http_code}\n' "${BRUCE_URL}/systeminfo"
curl -s -o /dev/null -w 'bad-bearer -> %{http_code}\n' -H "Authorization: Bearer wrong" "${BRUCE_URL}/systeminfo"
```

— both should print `401`.)

b. Make executable:

```bash
chmod +x tools/smoke_test_attacks.sh
```

c. Create `docs/bruce-companion-api.md` — copy the spec's §5 verbatim (the API Specification section of `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md`), prefixed with a header noting it's the source of truth and generated from this plan. Add at the top of the file:

```markdown
# Bruce Companion App — API Contract

**Source of truth:** `docs/superpowers/specs/2026-07-25-bruce-companion-app-design.md` §5. This document is the verbatim contract the `bruce-companion-app` repo vendors. Bump the version line below whenever this contract changes.

**Contract version:** 1.0 (initial — matches firmware commit after Task 11)

---

[Contents: copy spec §5.1 through §5.6 verbatim — Transport, Existing endpoints table, New CLI verbs table, Extended /systeminfo JSON, /ws EventFrame union, Evil Portal creds endpoint note]

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
```

- [ ] **Step 4: Run test to verify it passes**

Make sure the device is flashed with all patches and reachable:
```bash
BRUCE_URL=http://bruce.local BRUCE_TOKEN=<your_token> bash tools/smoke_test_attacks.sh
```
Expected: all `PASS` lines, summary `PASS=13 FAIL=0` (or thereabouts — exact count depends on negative-test curl form chosen). Negative auth lines print `401`.

Also verify the API doc:
```bash
ls -l docs/bruce-companion-api.md
head -20 docs/bruce-companion-api.md
```
Expected: file present and prefaced by the source-of-truth header.

- [ ] **Step 5: Commit**

```bash
git add tools/smoke_test_attacks.sh docs/bruce-companion-api.md
git commit -m "tools: add smoke_test_attacks.sh + freeze bruce-companion-api.md contract"
```

---

## Plan Summary

After Task 11 the firmware repo contains:
- 9 new CLI verbs accessible over `/cm` (and the BLE_API serial char, once enabled).
- Bearer-token auth (cookie path unchanged).
- A `/ws` event stream pushing `state`/`ble_progress`/`ble_result`/`log` frames.
- An extended `/systeminfo` that drives the app's capability-gating and status header.
- A `tools/smoke_test_attacks.sh` regression script.
- A frozen `docs/bruce-companion-api.md` contract for the `bruce-companion-app` repo to vendor.

The Mobile App Plan (to be written as a separate plan in the `bruce-companion-app` repo, or as a second plan in this repo's `docs/superpowers/plans/`) consumes the contract above and implements the 3 missions + Manual Console per the spec.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-25-bruce-firmware-patch.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. Each task's smoke test (the curl verifications) is the natural review gate.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints for review.

Which approach?