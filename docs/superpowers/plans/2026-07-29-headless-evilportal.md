# Headless Evil Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Evil Portal runnable in the background so BLE keeps answering while it runs, give it a remote stop verb and an unconditional duration cap, and thereby retire the stranding risk.

**Architecture:** `EvilPortal`'s constructor already skips its blocking `loop()` when `backgroundMode = true` (`src/modules/wifi/evil_portal.cpp:28-31`). A new fork-owned module heap-allocates a portal that way, and the serial command task — which already ticks every 10 ms — pumps its DNS server. The blocking on-device path is left completely untouched.

**Tech Stack:** C++17, Arduino-ESP32, FreeRTOS, ESPAsyncWebServer, SimpleCLI, PlatformIO, Unity (host tests), Python + bleak (bench).

**Source spec:** `docs/superpowers/specs/2026-07-29-headless-evilportal-design.md`

## Global Constraints

- Target env is `smoochiee-board`. Build with `pio run -e smoochiee-board`. Never claim a change works without a successful build.
- **FACTS OVER CLAIMS.** Cite `file:line` for code claims and device + date for measurements. Mark anything else UNVERIFIED. Keep VERIFIED distinct from SUSPECTED.
- **Comments explain *why*, never *what*.** Match the surrounding density. Do not narrate.
- **No AI attribution in commits.** No `Co-Authored-By`, no "Generated with" line.
- Commit style: `type(scope): imperative summary`, lowercase. The body explains the *symptom* that motivated the change.
- Formatting target is `.clang-format` (LLVM base, 4-space indent, 110 col). `clang-format` is **not installed on this machine** — match surrounding style by hand and do not fabricate a formatting step.
- **Never write to `serialDevice` from a path the operator did not invoke.** Unsolicited output lands inside another command's reply and breaks the `0x04` EOT framing. Asynchronous news goes to the event stream via `pushWsLog()`.
- **Never draw to the TFT from the serial task.** Sustained drawing from that task is the ISSUE-1 crash trigger.
- Bare `evilportal` must behave exactly as it does today. This change is purely additive.
- Line numbers below were read on 2026-07-29 and drift. Grep the symbol if one does not match.

## File Structure

| File | Responsibility |
|---|---|
| `src/modules/wifi/portal_cap.h` | **Create.** Header-only, pure, no Arduino deps. The duration-cap arithmetic — the only host-testable logic in this feature. |
| `test/test_portal_cap/test_portal_cap.cpp` | **Create.** Unity tests for the above under `[env:native]`. |
| `src/modules/wifi/evil_portal.h` / `.cpp` | **Modify.** Three small additions: `shutdown()`, `isReady()`, `getCredentialCount()`. Upstream file — keep the diff minimal. |
| `src/modules/wifi/evil_portal_bg.h` / `.cpp` | **Create.** Fork-owned. Owns the background portal's lifetime, the pump, the cap and the status string. |
| `src/core/serial_commands/attack_commands.cpp` | **Modify.** Flag args and callback branching. Fork-owned file. |
| `src/core/serialcmds.cpp` | **Modify.** One call: the pump site. |
| `tools/ble_spike/portal_bg.py` | **Create.** Bench script that proves the portal is genuinely headless. |

## A note on testing, stated plainly

Only Task 1 has a host test. Everything else touches Arduino, FreeRTOS, WiFi or AsyncWebServer, none of which builds under `[env:native]` (`platformio.ini` sets `test_build_src = no`, `framework =`). For Tasks 2–5 the verification is a successful `pio run -e smoochiee-board` plus the hardware bench in Task 7.

**This is a real coverage gap, not an oversight.** Do not invent host tests for the firmware tasks — a fake test that cannot exercise the code is worse than an honest gap. Task 1 exists because the cap arithmetic genuinely can be extracted and genuinely has a bug class worth testing.

---

### Task 1: Duration-cap arithmetic (host-tested)

**Files:**
- Create: `src/modules/wifi/portal_cap.h`
- Test: `test/test_portal_cap/test_portal_cap.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `bool portalCapExpired(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs)` and `uint32_t portalCapRemainingMs(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs)`. Task 3 calls both. `maxMs == 0` means "no cap" in both.

- [ ] **Step 1: Write the failing test**

Create `test/test_portal_cap/test_portal_cap.cpp`:

```cpp
#include "modules/wifi/portal_cap.h"
#include <unity.h>

void test_zero_cap_never_expires() {
    TEST_ASSERT_FALSE(portalCapExpired(0, 0xFFFFFFFFu, 0));
}

void test_not_expired_before_cap() {
    TEST_ASSERT_FALSE(portalCapExpired(1000, 1999, 1000));
}

void test_expired_exactly_at_cap() {
    TEST_ASSERT_TRUE(portalCapExpired(1000, 2000, 1000));
}

void test_expired_past_cap() {
    TEST_ASSERT_TRUE(portalCapExpired(1000, 5000, 1000));
}

// millis() wraps at ~49.7 days. Unsigned subtraction stays correct across the
// wrap; a naive nowMs < startedMs guard would not.
void test_expired_across_millis_rollover() {
    TEST_ASSERT_TRUE(portalCapExpired(0xFFFFFF00u, 0x00000100u, 0x100));
}

void test_not_expired_approaching_rollover() {
    TEST_ASSERT_FALSE(portalCapExpired(0xFFFFFF00u, 0xFFFFFF80u, 0x100));
}

void test_remaining_counts_down() {
    TEST_ASSERT_EQUAL_UINT32(400, portalCapRemainingMs(1000, 1600, 1000));
}

void test_remaining_clamps_at_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, portalCapRemainingMs(1000, 9999, 1000));
}

void test_remaining_is_zero_when_uncapped() {
    TEST_ASSERT_EQUAL_UINT32(0, portalCapRemainingMs(1000, 1600, 0));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_cap_never_expires);
    RUN_TEST(test_not_expired_before_cap);
    RUN_TEST(test_expired_exactly_at_cap);
    RUN_TEST(test_expired_past_cap);
    RUN_TEST(test_expired_across_millis_rollover);
    RUN_TEST(test_not_expired_approaching_rollover);
    RUN_TEST(test_remaining_counts_down);
    RUN_TEST(test_remaining_clamps_at_zero);
    RUN_TEST(test_remaining_is_zero_when_uncapped);
    return UNITY_END();
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `pio test -e native -f test_portal_cap`
Expected: FAIL at compile — `modules/wifi/portal_cap.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `src/modules/wifi/portal_cap.h`:

```cpp
#ifndef __PORTAL_CAP_H__
#define __PORTAL_CAP_H__

#include <stdint.h>

// Deliberately free of Arduino and FreeRTOS so it builds under [env:native],
// where almost nothing else in this feature can be tested.

// maxMs == 0 means no cap.
static inline bool portalCapExpired(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs) {
    if (maxMs == 0) return false;
    // Unsigned wraparound makes this correct across the ~49.7 day millis() rollover.
    return (uint32_t)(nowMs - startedMs) >= maxMs;
}

// Returns 0 when uncapped; callers must check maxMs to tell that from "expired".
static inline uint32_t portalCapRemainingMs(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs) {
    if (maxMs == 0) return 0;
    uint32_t elapsed = (uint32_t)(nowMs - startedMs);
    return elapsed >= maxMs ? 0 : maxMs - elapsed;
}

#endif
```

- [ ] **Step 4: Run the test and verify it passes**

Run: `pio test -e native -f test_portal_cap`
Expected: PASS, 9 tests, 0 failures.

- [ ] **Step 5: Confirm the existing suite still passes**

Run: `pio test -e native`
Expected: `test_byte_ring` and `test_portal_cap` both pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/wifi/portal_cap.h test/test_portal_cap/test_portal_cap.cpp
git commit -m "test(portal): rollover-safe duration cap arithmetic

The headless portal needs an unconditional time cap, because with ble api
off nothing can reach the device to stop it. The arithmetic is the one
piece of this feature that has no Arduino or FreeRTOS dependency, so it is
extracted and tested on the host rather than guessed at on hardware.

millis() wraps at roughly 49.7 days. Unsigned subtraction survives that;
an intuitive nowMs < startedMs guard does not, and the rollover cases are
what these tests exist for."
```

---

### Task 2: EvilPortal accessors and shutdown extraction

**Files:**
- Modify: `src/modules/wifi/evil_portal.h` (declarations, near `:28-50`)
- Modify: `src/modules/wifi/evil_portal.cpp` (`loop()` exit path at `:346-363`)

**Interfaces:**
- Consumes: nothing.
- Produces: `void EvilPortal::shutdown()`, `bool EvilPortal::isReady()`, `int EvilPortal::getCredentialCount()`. Task 3 calls all three.

There is **no host test for this task.** Verification is a clean build plus unchanged behaviour of the blocking path, confirmed on hardware in Task 7.

- [ ] **Step 1: Add a readiness flag to the header**

In `src/modules/wifi/evil_portal.h`, add to the `public:` block alongside `setup()`/`beginAP()`:

```cpp
    void shutdown(void);
    bool isReady() { return _ready; }
    int getCredentialCount() { return totalCapturedCredentials; }
```

and to the `private:` block, next to `bool _pendingWifiRestart = false;`:

```cpp
    bool _ready = false;
```

- [ ] **Step 2: Set the flag in the constructor**

In `src/modules/wifi/evil_portal.cpp`, the constructor currently ends:

```cpp
    if (!setup()) return;
    cleanlyStopWebUiForWiFiFeature();
    beginAP();
    if (!_backgroundMode) { loop(); }
}
```

Change to:

```cpp
    if (!setup()) return;
    cleanlyStopWebUiForWiFiFeature();
    beginAP();
    // setup() failing returns early above with no outward signal, so a background
    // caller holding this pointer cannot otherwise tell a live portal from a dead
    // object that never reached beginAP().
    _ready = true;
    if (!_backgroundMode) { loop(); }
}
```

- [ ] **Step 3: Extract shutdown() from the loop's exit path**

In `loop()`, the `exitPortal` branch currently reads:

```cpp
            if (exitPortal) {
                displayTextLine("Shutting down...");
                vTaskDelay(100 / portTICK_PERIOD_MS);

                webServer.end();
                vTaskDelay(200 / portTICK_PERIOD_MS);

                dnsServer->stop();
                vTaskDelay(100 / portTICK_PERIOD_MS);

                WiFi.mode(_originalWifiMode);
                vTaskDelay(100 / portTICK_PERIOD_MS);

                wifiDisconnect();
                vTaskDelay(100 / portTICK_PERIOD_MS);

                return;
            }
```

Replace with:

```cpp
            if (exitPortal) {
                displayTextLine("Shutting down...");
                vTaskDelay(100 / portTICK_PERIOD_MS);
                shutdown();
                return;
            }
```

and add the new method next to `processRequests()`:

```cpp
// The delays space out the teardown of four subsystems that share the WiFi
// stack; collapsing them has not been tested. Draws nothing, because the
// background path runs this from the serial task and sustained drawing from
// there is the ISSUE-1 crash trigger.
void EvilPortal::shutdown(void) {
    webServer.end();
    vTaskDelay(200 / portTICK_PERIOD_MS);

    dnsServer->stop();
    vTaskDelay(100 / portTICK_PERIOD_MS);

    WiFi.mode(_originalWifiMode);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    wifiDisconnect();
    vTaskDelay(100 / portTICK_PERIOD_MS);
}
```

- [ ] **Step 4: Build**

Run: `pio run -e smoochiee-board`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Confirm the blocking path is byte-for-byte equivalent in behaviour**

Read the diff and check by eye that the extracted `shutdown()` performs the same four operations in the same order with the same delays, and that `displayTextLine` stayed in `loop()`. Nothing else in `loop()` changed.

Run: `git diff src/modules/wifi/evil_portal.cpp`

- [ ] **Step 6: Commit**

```bash
git add src/modules/wifi/evil_portal.h src/modules/wifi/evil_portal.cpp
git commit -m "refactor(portal): expose teardown, readiness and credential count

Groundwork for running the portal in the background. Three gaps block that
today.

The teardown sequence was inline in loop()'s exit branch, so nothing but an
on-device Esc could reach it. It is now shutdown(), with the screen write
left behind in loop() — the background path runs on the serial task, and
sustained drawing from there is what crashes the device.

The constructor's early return on setup() failure was silent: beginAP()
never runs and the caller cannot tell. isReady() makes that visible instead
of handing back a pointer to a portal that is not on air.

hasCredentials() is a bool, so it can only ever transition once. Detecting
a second capture needs the count."
```

---

### Task 3: The background portal module

**Files:**
- Create: `src/modules/wifi/evil_portal_bg.h`
- Create: `src/modules/wifi/evil_portal_bg.cpp`

**Interfaces:**
- Consumes: `EvilPortal(ssid, channel, deauth, verifyPwd, autoMode, backgroundMode, templateFile)`, `EvilPortal::shutdown()`, `isReady()`, `getCredentialCount()`, `processRequests()` (Task 2); `portalCapExpired()`, `portalCapRemainingMs()` (Task 1); `pushWsLog(const String&, const char*)` and `setDeviceState(const String&)` from `core/wifi/ws_events.h:15-16`; `radioLargestDmaBlock()` from `core/radio_mem.h:25`.
- Produces: `evilPortalBgStart(const String&, uint8_t, const String&, uint32_t)`, `evilPortalBgStop()`, `evilPortalBgRunning()`, `evilPortalBgTick()`, `evilPortalBgStatus()`. Task 4 calls all but `Tick`; Task 5 calls `Tick`.

No host test. Verification is a clean build plus Task 7.

- [ ] **Step 1: Write the header**

Create `src/modules/wifi/evil_portal_bg.h`:

```cpp
#ifndef __EVIL_PORTAL_BG_H__
#define __EVIL_PORTAL_BG_H__

#include <Arduino.h>

// A portal that does not hold the serial task. The blocking EvilPortal runs its
// own while(true) from inside the CLI callback, so while it is up no BLE command
// is parsed at all and, with the WebUI torn down on entry, the device has no
// remote control surface left.

bool evilPortalBgStart(const String &ssid, uint8_t channel, const String &templateFile, uint32_t maxSeconds);
bool evilPortalBgStop();
bool evilPortalBgRunning();
String evilPortalBgStatus();

// Pumped from the serial command task; see the call site in serialcmds.cpp.
void evilPortalBgTick();

#endif
```

- [ ] **Step 2: Write the implementation**

Create `src/modules/wifi/evil_portal_bg.cpp`:

```cpp
#if !defined(LITE_VERSION)
#include "evil_portal_bg.h"
#include "core/radio_mem.h"
#include "core/wifi/ws_events.h"
#include "modules/wifi/evil_portal.h"
#include "modules/wifi/portal_cap.h"
#include <globals.h>
#include <new>

static EvilPortal *bgPortal = nullptr;
static uint32_t bgStartedMs = 0;
static uint32_t bgMaxMs = 0;
static int bgLastCredCount = 0;
static String bgSsid = "";
static uint8_t bgChannel = 0;

bool evilPortalBgRunning() { return bgPortal != nullptr; }

// Whether an armed BLE stack and the portal AP fit together is the open question
// this feature cannot answer by reasoning, so every reply carries the numbers.
static String heapReport() {
    return String("free_heap:") + String(ESP.getFreeHeap()) + " dma_block:" +
           String((unsigned)radioLargestDmaBlock());
}

// announceOnCli is false for the duration cap, which fires from the tick while the
// serial task may be mid-command. Writing to serialDevice there would land inside
// another reply and break its 0x04 EOT framing.
static void stopPortal(bool announceOnCli) {
    bgPortal->shutdown();
    delete bgPortal;
    bgPortal = nullptr;
    setDeviceState("idle");
    pushWsLog("portal stopped: " + bgSsid, "info");
    if (announceOnCli) serialDevice->println("portal '" + bgSsid + "' stopped. " + heapReport());
    bgSsid = "";
    bgChannel = 0;
    bgMaxMs = 0;
    bgLastCredCount = 0;
}

bool evilPortalBgStart(
    const String &ssid, uint8_t channel, const String &templateFile, uint32_t maxSeconds
) {
    if (bgPortal != nullptr) {
        serialDevice->println("ERROR: portal already running on '" + bgSsid + "'");
        return false;
    }

    // The gateway default is applied by the caller in attack_commands.cpp, which
    // runs ahead of both the blocking and the background path. Do not repeat it.

    EvilPortal *portal =
        new (std::nothrow) EvilPortal(ssid, channel, false, false, true, true, templateFile);
    if (portal == nullptr) {
        serialDevice->println("ERROR: out of memory starting portal. " + heapReport());
        return false;
    }
    if (!portal->isReady()) {
        delete portal;
        serialDevice->println("ERROR: portal setup failed, not on air. " + heapReport());
        return false;
    }

    bgPortal = portal;
    bgSsid = ssid;
    bgChannel = channel;
    bgStartedMs = millis();
    bgMaxMs = maxSeconds * 1000UL;
    bgLastCredCount = 0;

    setDeviceState("portal");
    pushWsLog("portal started: " + ssid + " ch" + String(channel), "info");
    serialDevice->println(
        "portal '" + ssid + "' ch" + String(channel) + " started, cap " +
        (maxSeconds ? String(maxSeconds) + "s" : String("unlimited")) + ". " + heapReport()
    );
    return true;
}

bool evilPortalBgStop() {
    if (bgPortal == nullptr) {
        serialDevice->println("no background portal running");
        return false;
    }
    stopPortal(true);
    return true;
}

String evilPortalBgStatus() {
    if (bgPortal == nullptr) return "portal: stopped";
    uint32_t now = millis();
    String out = "portal: running ssid:" + bgSsid + " ch:" + String(bgChannel) +
                 " uptime_s:" + String((now - bgStartedMs) / 1000) +
                 " creds:" + String(bgPortal->getCredentialCount());
    if (bgMaxMs == 0) out += " cap:unlimited";
    else out += " cap_remaining_s:" + String(portalCapRemainingMs(bgStartedMs, now, bgMaxMs) / 1000);
    return out + " " + heapReport();
}

void evilPortalBgTick() {
    if (bgPortal == nullptr) return;

    bgPortal->processRequests();

    int creds = bgPortal->getCredentialCount();
    if (creds > bgLastCredCount) {
        bgLastCredCount = creds;
        pushWsLog("portal captured credentials (" + String(creds) + " total)", "warn");
    }

    if (portalCapExpired(bgStartedMs, millis(), bgMaxMs)) {
        pushWsLog("portal duration cap reached: " + bgSsid, "info");
        stopPortal(false);
    }
}
#endif
```

- [ ] **Step 3: Build**

Run: `pio run -e smoochiee-board`
Expected: `[SUCCESS]`. Nothing calls this module yet, so a link error here means a missing include, not a wiring problem.

- [ ] **Step 4: Commit**

```bash
git add src/modules/wifi/evil_portal_bg.h src/modules/wifi/evil_portal_bg.cpp
git commit -m "feat(portal): background portal lifetime, pump and duration cap

Holds an EvilPortal built with backgroundMode = true, which the constructor
already honours by skipping loop(). The serial task therefore stays free and
BLE keeps answering while the portal is up.

Every failure path prints something distinguishable. Start refuses a second
portal by name, reports an allocation failure with the heap figures, and
catches the constructor's silent setup() failure through isReady() rather
than handing back a pointer to a portal that never reached the air. Replies
carry free heap and the largest DMA block because whether BLE and the portal
AP coexist is still unmeasured.

The duration cap stops the portal without writing to serialDevice. It fires
from the tick, where the serial task may be mid-command, and an unsolicited
write there would land inside another reply's EOT framing."
```

---

### Task 4: Verb wiring

**Files:**
- Modify: `src/core/serial_commands/attack_commands.cpp` (`evilportalCmdCallback` at `:35-55`, registration at `:182-185`)

**Interfaces:**
- Consumes: `evilPortalBgStart/Stop/Status` (Task 3).
- Produces: the `evilportal -bg / -off / -status / -duration` CLI surface.

No host test. Verification is a clean build plus Task 7.

- [ ] **Step 1: Add the include**

At the top of `src/core/serial_commands/attack_commands.cpp`, next to the existing `#include "modules/wifi/evil_portal.h"`:

```cpp
#include "modules/wifi/evil_portal_bg.h"
```

- [ ] **Step 2: Branch the callback on the flags**

Replace `evilportalCmdCallback` with:

```cpp
uint32_t evilportalCmdCallback(cmd *c) {
    Command cmd(c);

    // Flags are handled before the start path so -off and -status can never
    // bring a portal up as a side effect.
    if (cmd.getArgument("off").isSet()) { return evilPortalBgStop(); }
    if (cmd.getArgument("status").isSet()) {
        serialDevice->println(evilPortalBgStatus());
        return true;
    }

    String ssid = cmd.getArgument("ssid").getValue();
    String chStr = cmd.getArgument("channel").getValue();
    String templateFile = cmd.getArgument("template").getValue();
    ssid.trim();
    chStr.trim();
    templateFile.trim();
    if (ssid.isEmpty()) ssid = "Free Wifi";
    uint8_t channel = (uint8_t)chStr.toInt();
    if (channel < 1 || channel > 13) channel = 6;
    // Default gateway to 192.168.4.1 for phone captive-portal compatibility
    // (172.0.0.1 breaks Android/iOS auto-detection — phones expect 192.168.4.1)
    if (bruceConfig.evilPortalGatewayIp.isEmpty()) {
        bruceConfig.evilPortalGatewayIp = "192.168.4.1";
    }

    if (cmd.getArgument("bg").isSet()) {
        String durStr = cmd.getArgument("duration").getValue();
        durStr.trim();
        long duration = durStr.toInt();
        if (duration < 0) duration = 0;
        return evilPortalBgStart(ssid, channel, templateFile, (uint32_t)duration);
    }

    setDeviceState("portal");
    EvilPortal(ssid, channel, false, false, true, false, templateFile);
    setDeviceState("idle");
    return true;
}
```

- [ ] **Step 3: Register the new arguments**

In `createAttackCommands`, the block currently reads:

```cpp
    Command evilportal = cli->addCommand("evilportal", evilportalCmdCallback);
    evilportal.addPosArg("ssid", "Free Wifi");
    evilportal.addPosArg("channel", "6");
    evilportal.addPosArg("template", "");
```

Append:

```cpp
    evilportal.addFlagArg("bg");
    evilportal.addFlagArg("off");
    evilportal.addFlagArg("status");
    // 0 disables the cap. The default is deliberately finite: with ble api off
    // nothing can reach the device to stop a portal, so the clock is the only
    // recovery path.
    evilportal.addArg("duration", "600");
```

- [ ] **Step 4: Build**

Run: `pio run -e smoochiee-board`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Commit**

```bash
git add src/core/serial_commands/attack_commands.cpp
git commit -m "feat(portal): evilportal -bg, -off, -status and -duration

Mirrors webui's flag idiom so the app meets one convention rather than two.
Bare evilportal is unchanged and still blocks on the device.

Mixing positional and flag arguments has no precedent in this tree, so the
SimpleCLI parser was read rather than assumed: a dash-prefixed word binds by
name and addPosArg with a default builds an optional argument, so
evilportal -off parses with ssid, channel and template left at their
defaults. An SSID that begins with a dash is read as a flag name and
returns unknown argument; that limitation is accepted, not worked around.

-duration defaults to 600 seconds rather than unlimited. With ble api off
the portal destroys the WebUI on entry and nothing can reach the device, so
the clock is the only thing that can end it."
```

---

### Task 5: Pump wiring

**Files:**
- Modify: `src/core/serialcmds.cpp` (top of `handleSerialCommands`, currently at `:48`; the early return it must precede is at `:71`)

**Interfaces:**
- Consumes: `evilPortalBgTick()` (Task 3).
- Produces: nothing.

No host test. Verification is a clean build plus Task 7 — specifically bench step 2, which is what proves this wiring works at all.

- [ ] **Step 1: Add the include**

At the top of `src/core/serialcmds.cpp`, inside the existing `#if !defined(LITE_VERSION)` block that already includes `core/wifi/ws_events.h`:

```cpp
#include "modules/wifi/evil_portal_bg.h"
```

- [ ] **Step 2: Call the tick at the very top of handleSerialCommands**

The function currently begins:

```cpp
void handleSerialCommands(SerialCli &serialCli) {
    CmdPacket packet;
```

Change to:

```cpp
void handleSerialCommands(SerialCli &serialCli) {
#if !defined(LITE_VERSION)
    // Pumped from this task because it already ticks every 10 ms and, unlike the
    // main loop, never sinks into an on-device menu. It must run before the
    // hasLine() early return below, or it would only tick when a command
    // happened to be waiting.
    evilPortalBgTick();
#endif

    CmdPacket packet;
```

- [ ] **Step 3: Verify placement by reading, not by assuming**

Run: `grep -n "evilPortalBgTick\|hasLine\|redrawUnlessNavigation" src/core/serialcmds.cpp`
Expected: the `evilPortalBgTick()` line number is **lower** than the `hasLine` line number. If it is not, the pump is behind the early return and will effectively never run.

- [ ] **Step 4: Build**

Run: `pio run -e smoochiee-board`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Commit**

```bash
git add src/core/serialcmds.cpp
git commit -m "feat(portal): pump the background portal from the serial task

The DNS server needs servicing for captive-portal detection to fire. The
AsyncWebServer drives itself, so a starved pump costs the automatic popup,
not page serving.

This task already runs every 10 ms and does not disappear into loopOptions
the way the main loop does when someone opens a menu on the device. A
dedicated task was rejected because its stack comes out of internal DRAM,
which is the resource the whole BLE-plus-WiFi problem is about.

The call sits above the hasLine() early return. Below it, the portal would
only be pumped when a command happened to be waiting."
```

---

### Task 6: Bench script

**Files:**
- Create: `tools/ble_spike/portal_bg.py`

**Interfaces:**
- Consumes: the CLI surface from Task 4.
- Produces: nothing consumed by later code. Task 7 runs it.

- [ ] **Step 1: Write the script**

Create `tools/ble_spike/portal_bg.py`:

```python
#!/usr/bin/env python3
"""Prove the headless Evil Portal does not hold the serial task.

The load-bearing assertion is step 2: today the portal blocks the CLI for its
entire life, so a reply to `uptime` while a portal is up means it is genuinely
headless. Everything else is secondary.

Discovery is by service UUID, never by name — name discovery has produced four
false "device is bricked" conclusions on this project.

Usage:  python3 portal_bg.py [--ssid PortalTest] [--channel 6] [--duration 300]
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EVT = "d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self.done = asyncio.Event()
        self.events = []

    def on_cli(self, _h, data):
        self.buf.extend(data)
        if EOT in data:
            self.done.set()

    def on_evt(self, _h, data):
        text = data.decode("utf-8", "replace").strip()
        self.events.append(text)
        print(f"    [event] {text}")

    async def send(self, cmd, timeout=20.0):
        self.buf.clear()
        self.done.clear()
        await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
        try:
            await asyncio.wait_for(self.done.wait(), timeout)
        except asyncio.TimeoutError:
            return None  # no EOT: either blocked or the reply could not be allocated
        return self.buf.replace(bytes([EOT]), b"").decode("utf-8", "replace").strip()


async def main(args):
    print(f"scanning for service {SVC} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC.lower() in [u.lower() for u in ad.service_uuids], timeout=15.0
    )
    if dev is None:
        print("FAIL: no device advertising the Bruce service UUID")
        return 1
    print(f"found {dev.address}")

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_cli)
        await client.start_notify(EVT, link.on_evt)

        print(f"\n[1] start headless portal '{args.ssid}' ch{args.channel} cap {args.duration}s")
        reply = await link.send(
            f"evilportal {args.ssid} {args.channel} -bg -duration {args.duration}"
        )
        print(f"    {reply!r}")
        if reply is None or "ERROR" in (reply or ""):
            print("FAIL: portal did not start")
            return 1

        # The whole point of the change. Before it, this call could not be answered.
        print("\n[2] send `uptime` over BLE while the portal is up  <-- load-bearing")
        reply = await link.send("uptime", timeout=15.0)
        if reply is None:
            print("FAIL: no reply. The serial task is still held; the portal is NOT headless.")
            return 1
        print(f"    PASS: {reply!r}")

        print("\n[3] status")
        print(f"    {await link.send('evilportal -status')!r}")

        print(f"\n[4] join AP '{args.ssid}' from another machine, browse 192.168.4.1,")
        print("    submit credentials, then press Enter here.")
        await asyncio.get_running_loop().run_in_executor(None, input)
        print(f"    {await link.send('evilportal -status')!r}")
        caps = [e for e in link.events if "captured" in e]
        print(f"    capture events seen: {len(caps)}")

        print("\n[5] stop over BLE")
        print(f"    {await link.send('evilportal -off')!r}")

        print("\n[6] confirm BLE still alive after stop")
        reply = await link.send("uptime")
        if reply is None:
            print("FAIL: no reply after stop")
            return 1
        print(f"    PASS: {reply!r}")

        print("\n[7] -off with nothing running should say so, not claim success")
        print(f"    {await link.send('evilportal -off')!r}")

    print("\ndone")
    return 0


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--ssid", default="PortalTest")
    p.add_argument("--channel", type=int, default=6)
    p.add_argument("--duration", type=int, default=300)
    sys.exit(asyncio.run(main(p.parse_args())))
```

- [ ] **Step 2: Check it parses**

Run: `python3 -m py_compile tools/ble_spike/portal_bg.py`
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add tools/ble_spike/portal_bg.py
git commit -m "tools: bench the headless portal over BLE

Step 2 is the whole test. Before this change the portal held the serial task
for its entire life, so a reply to uptime while a portal is up is the proof
that it is genuinely headless; the remaining steps only characterise a
feature that step 2 has already established exists.

Discovers by service UUID. Name discovery has produced four false bricked
conclusions on this device.

A missing EOT is reported rather than hidden, because an empty BLE reply
means the device could not allocate an answer, not that nothing happened."
```

---

### Task 7: Hardware verification and documentation

**Files:**
- Modify: `docs/KNOWN_ISSUES.md` (ISSUE-19, ISSUE-1, ISSUE-24)
- Modify: `docs/TEST_STATUS.md` (Shippable / Broken / Constraints tables)
- Modify: `docs/bruce-companion-api.md` (the `evilportal` entry)
- Modify: `BRUCELINK.md` if any claim there is contradicted by a measurement

**Interfaces:** consumes everything above.

This task needs the operator physically present — the board is reset by hand. **Batch the flash.** Uncommitted work from earlier in the session (the ISSUE-24 repaint fix in `serialcmds.cpp` and the Touch.cpp transaction pair) should ride in the same flash.

- [ ] **Step 1: Flash**

Run: `pio run -e smoochiee-board -t upload`
Then record the new ELF hash — every later backtrace decode is fiction without it:
Run: `grep -a -o 'ELF file SHA256.\{0,20\}' .pio/build/smoochiee-board/firmware.bin | head -1`

- [ ] **Step 2: Start the console capture**

Run: `python3 tools/ble_spike/usbwatch2.py > /tmp/portal_bg_console.log &`
**Exactly one instance.** Two silently split the stream and both look empty. To kill it later use `pkill -f "ble_spike/usbwatch2[.]py"` — a bare `pkill -f usbwatch2` matches its own command line and kills the calling shell.

- [ ] **Step 3: Run the bench**

Run: `python3 tools/ble_spike/portal_bg.py --ssid PortalTest --channel 6 --duration 300`

Record verbatim: the heap and DMA figures from step 1, whether step 2 replied, and whether step 4's page actually served with BLE armed.

- [ ] **Step 4: Verify ISSUE-24 in the same session**

With the portal stopped, dispatch a drawing verb over HTTP and confirm the screen returns to the menu rather than freezing on the verb's last frame. This is the fix already sitting in `serialcmds.cpp`.

Sequence: `webui -bg` over BLE → `ble api off` over BLE → HTTP login (form fields `username`/`password`, **not** JSON) → `POST /cm?cmnd=<a verb that draws>` → watch the screen → `POST /cm?cmnd=ble api on` to get BLE back.

- [ ] **Step 5: Retest ISSUE-1's evilportal case under load**

The portal no longer draws from the serial task, so this is testable for the first time. Start a headless portal, then apply sustained load — associate a station and drive requests — while the main loop repaints its status bar on the 30 s timer. Watch `/tmp/portal_bg_console.log` for `assert failed` or `Backtrace:`.

**An idle portal proves nothing.** Report the result as "no crash observed in N minutes under load", never as "safe".

- [ ] **Step 6: Update the docs with what actually happened**

Write only what was measured, with device and date. In particular:
- If bench step 4 served pages with BLE armed, that is a **new** finding and contradicts the current "transports alternate" framing — record it carefully and do not overgeneralise from one run. If it starved, record that too; the prediction was made in advance in the spec.
- ISSUE-19: narrow the "no remote rescue" claim to the verbs it still applies to.
- ISSUE-24: move to §Resolved with the commit and the proving test only if step 4 actually passed.
- ISSUE-1: attach the new evidence; **keep it open** — a probabilistic result against a race is not proof.
- TEST_STATUS: move Evil Portal only as far as the evidence supports.

- [ ] **Step 7: Commit**

```bash
git add docs/ BRUCELINK.md
git commit -m "docs: what the headless portal actually did on hardware

<Replace this body with the measurements: device, date, ELF hash, the heap
and DMA figures, whether BLE answered while the portal was up, whether the
page served with BLE armed, and how long the ISSUE-1 retest ran under load
before it was stopped. Say plainly what was not tested.>"
```

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task: architecture and the pump site → Tasks 3 and 5; the three `evil_portal.cpp` changes → Task 2; verb surface and parsing → Task 4; duration cap → Tasks 1 and 3; data flow and app feedback (`state` frames already exist, `log` on capture, `-status`) → Tasks 3 and 4; the five error-handling rows → Task 3 Step 2; testing → Tasks 6 and 7; success criteria → Task 7. "Deliberately not doing" (`~EvilPortal()` and karma's leak) is carried into no task by design, and is recorded in the spec.

**Placeholders.** One intentional placeholder remains: the Task 7 Step 7 commit body, which cannot be written before the measurements exist and is marked to be replaced. Everything else contains the actual content.

**Type consistency.** `evilPortalBgStart/Stop/Running/Tick/Status` are spelled identically in the header (Task 3 Step 1), the implementation (Step 2), the callback (Task 4 Step 2) and the pump (Task 5 Step 2). `portalCapExpired`/`portalCapRemainingMs` match between Task 1 and Task 3. `shutdown()`/`isReady()`/`getCredentialCount()` match between Task 2 and Task 3. `pushWsLog(const String&, const char*)` and `setDeviceState(const String&)` match `ws_events.h:15-16`.

**Known gap, stated rather than hidden.** Tasks 2–5 have no automated test. The firmware surface is untestable on the host and this plan does not pretend otherwise; Task 7 is where they are actually verified, and until it passes, none of them should be described as working.
