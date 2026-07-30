# RC3 — WebUI start gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `webui` refuse to start when it cannot, detect it when the HTTP server fails to listen anyway, and report either outcome on channels that exist on this board.

**Architecture:** Four ordered decision points on the `webui` path — a DMA gate in `wifiConnectMenu`'s AP case, an honest `softAP()` return in `_setupAP()`, a second DMA gate at the point `RAM_LOG("webui pre-alloc")` already samples, and an exact `AsyncWebServer::state() == LISTEN` check after `begin()`. `isWebUIActive = true` moves behind the last of them, `startWebUi()` returns `bool`, and every outcome goes through one reporting helper backed by a pure host-tested formatter.

**Tech Stack:** C++17, PlatformIO, Arduino-ESP32 (ESP32-S3), ESPAsyncWebServer/AsyncTCP, Unity for host tests.

**Spec:** `docs/superpowers/specs/2026-07-30-webui-start-gate-design.md` (commit `ac41f9a0`). Read it before Task 1 — it carries the evidence and three corrections the design depends on.

**Baseline:** HEAD `ac41f9a0` on `main`, tree clean, pushed. `pio run -e smoochiee-board` SUCCESS. `pio test -e native` **28/28** (`test_byte_ring` 12, `test_portal_cap` 9, `test_crash_report` 7). Device `1C:DB:D4:5E:D7:39` on `/dev/ttyACM0` runs ELF `fabcc0003` = HEAD, coredump partition empty, BLE armed.

## Global Constraints

Every task's requirements implicitly include these.

- **Reuse `RADIO_WIFI_MIN_DMA_BLOCK` (15,360, `radio_mem.h:29`) unchanged. Invent no new threshold.** The spec rejects a value between 18,420 and 19,444 as single-observation.
- **`log_e` is the only compiled log level.** `-DCORE_DEBUG_LEVEL=1` (`boards/smoochiee-board/smoochiee-board.ini:21`) strips everything below ERROR. Not a stylistic choice.
- **Never `Serial` for anything the app or the bench must read.** `Serial` is the native USB-CDC port; the bench reads the UART bridge (`ram_profile.cpp:9-18`, ISSUE-22). Use `serialDevice->` for CLI replies (ISSUE-42 was exactly this bug), `log_e` for the console, `RAM_LOGF` when a non-error line must reach the console.
- **`displayError(txt, false)` — never `true`.** `waitKeyPress` spins on `while (!check(AnyKeyPress))` (`display.cpp:322`) and would hold the serial task with no BLE dismissal.
- **Comments explain *why*, never *what*.** Match the surrounding density; do not add narration. Every comment in this plan's code blocks carries a reason — keep them, they are the deliverable as much as the code.
- **Commits:** `type(scope): imperative summary`, lowercase. The body explains the **symptom** that motivated the change. **No `Co-Authored-By:`, no "Generated with" line** — configured off globally.
- **Formatting:** LLVM base, 4-space indent, 110 columns. **`clang-format` is NOT installed on this machine** — match surrounding style by hand; do not attempt to run it.
- **`pkill -f platformio` kills your own shell** (exit 144). Use `pgrep -f '[p]latformio'` then kill by bare PID. `fuser -v /dev/ttyACM0` is the clean port check.
- **FACTS OVER CLAIMS.** Cite `file:line` for code, device + date for measurements. Report "no failure observed in N s", never "safe". If a step is skipped or blocked, say so explicitly.
- **Device and tree must stay aligned.** Any code commit invalidates the flashed ELF; reflash before trusting `crashlog`, or it reports `match=NO`.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/core/wifi/webui_gate.h` | **Create.** Pure decision predicates + one-line report formatter. No Arduino/FreeRTOS/IDF, so `[env:native]` can build it. | 1 |
| `test/test_webui_gate/test_webui_gate.cpp` | **Create.** Host tests for the above. | 1 |
| `src/core/wifi/wifi_common.cpp` | **Modify** `_setupAP()` (:142-150) and `wifiConnectMenu()`'s `WIFI_AP` case (:182-185). Gates A and B. | 2 |
| `src/core/wifi/webInterface.cpp` | **Modify** `startWebUi()` (:752-802). Gates C and D, the unwind, the reporting helper. | 3 |
| `src/core/wifi/webInterface.h` | **Modify** `startWebUi`'s declaration (:28). | 3, 5 |
| `src/core/radio_mem.h` | **Modify** three `Serial` diagnostics (:48, :62, :67) → `log_e`. | 4 |
| `src/core/serial_commands/wifi_commands.cpp` | **Modify** `webuiCallback` (:55-82) and the verb's flag args (:130-133). | 3, 5 |
| `docs/KNOWN_ISSUES.md`, `docs/bruce-companion-api.md`, `docs/TEST_STATUS.md`, `BRUCELINK.md` | **Modify.** Register, contract (→ 2.5), coverage map, agent memory. | 7 |

Tasks 1–5 are code and build-verifiable offline. Task 6 needs the board. Task 7 comes last so the docs record measured results rather than expectations.

---

### Task 1: The pure gate module and its host tests

**Files:**
- Create: `src/core/wifi/webui_gate.h`
- Test: `test/test_webui_gate/test_webui_gate.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class WebUiStartResult : uint8_t { Started, WifiBringUpFailed, RefusedLowDmaPreAlloc, FailedNotListening }`; `struct WebUiStartReport { WebUiStartResult result; uint32_t dmaBlock; uint32_t required; uint8_t tcpState; bool apMode; }`; `bool webUiListening(uint8_t)`; `bool webUiDmaSufficient(uint32_t, uint32_t)`; `const char *webUiResultSlug(WebUiStartResult)`; `std::string formatWebUiStartReport(const WebUiStartReport &)`. All `static inline` in the header, mirroring `crash_report.h`.

**Why the enum has four values and not five.** An earlier draft split the pre-WiFi failure into "low DMA" and "AP failed". `startWebUi` cannot distinguish them — `wifiConnectMenu` returns a bare `bool` — so reporting either would be a guess. One honest slug, `wifi_bringup_failed`; gates A and B each emit their own `log_e` saying which.

- [ ] **Step 1: Write the failing test**

Create `test/test_webui_gate/test_webui_gate.cpp`:

```cpp
#include "core/wifi/webui_gate.h"
#include <string.h>
#include <unity.h>

// The gates themselves read the heap and touch WiFi, so only the predicates and
// the formatter can run here. Figures below are ISSUE-12's measured runs.

static WebUiStartReport baseReport() {
    WebUiStartReport r;
    r.result = WebUiStartResult::Started;
    r.dmaBlock = 6900;
    r.required = 15360;
    r.tcpState = 1;
    r.apMode = true;
    return r;
}

void test_listening_only_on_lwip_listen() {
    TEST_ASSERT_TRUE(webUiListening(1));  // LISTEN
    TEST_ASSERT_FALSE(webUiListening(0)); // CLOSED: a failed begin() leaves _pcb null
    TEST_ASSERT_FALSE(webUiListening(4)); // ESTABLISHED is not a listening socket
}

void test_dma_threshold_is_inclusive() {
    TEST_ASSERT_TRUE(webUiDmaSufficient(15360, 15360));
    TEST_ASSERT_FALSE(webUiDmaSufficient(15359, 15360));
    TEST_ASSERT_FALSE(webUiDmaSufficient(1844, 15360)); // 2026-07-29 failing run
    // 2026-07-30 run 2: clears the gate and still failed downstream. Pinned so a
    // later change cannot quietly "fix" that by inventing a higher threshold.
    TEST_ASSERT_TRUE(webUiDmaSufficient(18420, 15360));
}

void test_every_result_has_a_distinct_slug() {
    const char *a = webUiResultSlug(WebUiStartResult::Started);
    const char *b = webUiResultSlug(WebUiStartResult::WifiBringUpFailed);
    const char *c = webUiResultSlug(WebUiStartResult::RefusedLowDmaPreAlloc);
    const char *d = webUiResultSlug(WebUiStartResult::FailedNotListening);
    TEST_ASSERT_EQUAL_STRING("started", a);
    TEST_ASSERT_EQUAL_STRING("wifi_bringup_failed", b);
    TEST_ASSERT_EQUAL_STRING("low_dma_pre_alloc", c);
    TEST_ASSERT_EQUAL_STRING("not_listening", d);
}

void test_report_renders_every_field_on_one_line() {
    WebUiStartReport r = baseReport();
    r.result = WebUiStartResult::RefusedLowDmaPreAlloc;
    r.dmaBlock = 1844;
    r.tcpState = 0;
    std::string s = formatWebUiStartReport(r);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "low_dma_pre_alloc"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "dma=1844"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "required=15360"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "mode=ap"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "tcp_state=0"));
    // Single line: a BLE reply can truncate (ISSUE-16) and the first line must carry
    // the whole verdict.
    TEST_ASSERT_NULL(strchr(s.c_str(), '\n'));
    TEST_ASSERT_NULL(strchr(s.c_str(), '\r'));
}

void test_report_distinguishes_sta_from_ap() {
    WebUiStartReport r = baseReport();
    r.apMode = false;
    TEST_ASSERT_NOT_NULL(strstr(formatWebUiStartReport(r).c_str(), "mode=sta"));
    r.apMode = true;
    TEST_ASSERT_NOT_NULL(strstr(formatWebUiStartReport(r).c_str(), "mode=ap"));
}

void test_success_report_carries_the_post_begin_dma_figure() {
    // Reported on success too, and deliberately not gated on: ISSUE-12 records only
    // as SUSPECTED that this figure predicts the outcome (6900/6644 served, 6132 did
    // not) on one set of three runs.
    WebUiStartReport r = baseReport();
    std::string s = formatWebUiStartReport(r);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "started"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "dma=6900"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "tcp_state=1"));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_listening_only_on_lwip_listen);
    RUN_TEST(test_dma_threshold_is_inclusive);
    RUN_TEST(test_every_result_has_a_distinct_slug);
    RUN_TEST(test_report_renders_every_field_on_one_line);
    RUN_TEST(test_report_distinguishes_sta_from_ap);
    RUN_TEST(test_success_report_carries_the_post_begin_dma_figure);
    return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
pio test -e native -f test_webui_gate
```

Expected: compile FAILURE — `core/wifi/webui_gate.h: No such file or directory`. `[env:native]` already passes `-I src` (`platformio.ini`), so the include path is right and only the header is missing.

- [ ] **Step 3: Write the minimal implementation**

Create `src/core/wifi/webui_gate.h`:

```cpp
#ifndef __WEBUI_GATE_H__
#define __WEBUI_GATE_H__

#include <stdint.h>
#include <stdio.h>
#include <string>

// Deliberately free of Arduino, FreeRTOS and ESP-IDF so it builds under
// [env:native]. lwip/tcpbase.h and esp_heap_caps.h cannot come in here; the
// caller reads the socket state and the heap and copies the figures into the
// view below. Same arrangement as core/crash_report.h.

enum class WebUiStartResult : uint8_t {
    Started = 0,
    // Gate A or gate B. wifiConnectMenu() returns a bare bool, so distinguishing
    // "refused for memory" from "softAP failed" here would be a guess; each gate
    // logs its own reason instead.
    WifiBringUpFailed,
    RefusedLowDmaPreAlloc,
    FailedNotListening,
};

struct WebUiStartReport {
    WebUiStartResult result;
    uint32_t dmaBlock; // largest contiguous DMA-capable block at the deciding moment
    uint32_t required; // carried so a truncated line is still self-describing
    uint8_t tcpState;  // AsyncWebServer::state() as read; 0 where not applicable
    bool apMode;
};

// lwIP tcp_state::LISTEN. A literal rather than an include, to keep this header
// buildable off-device; the value is fixed by the on-wire TCP state machine.
static constexpr uint8_t WEBUI_TCP_LISTEN = 1;

static inline bool webUiListening(uint8_t tcpState) { return tcpState == WEBUI_TCP_LISTEN; }

static inline bool webUiDmaSufficient(uint32_t block, uint32_t required) { return block >= required; }

static inline const char *webUiResultSlug(WebUiStartResult r) {
    switch (r) {
        case WebUiStartResult::Started: return "started";
        case WebUiStartResult::WifiBringUpFailed: return "wifi_bringup_failed";
        case WebUiStartResult::RefusedLowDmaPreAlloc: return "low_dma_pre_alloc";
        case WebUiStartResult::FailedNotListening: return "not_listening";
    }
    return "unknown";
}

// One line, one field per token, because this is read over BLE where a reply can
// truncate (ISSUE-16) and a half-received line must still be useful.
static inline std::string formatWebUiStartReport(const WebUiStartReport &r) {
    char buf[96];
    std::string out = "webui: ";
    out += webUiResultSlug(r.result);
    out += r.apMode ? " mode=ap" : " mode=sta";
    snprintf(
        buf,
        sizeof(buf),
        " dma=%u required=%u tcp_state=%u",
        (unsigned)r.dmaBlock,
        (unsigned)r.required,
        (unsigned)r.tcpState
    );
    out += buf;
    return out;
}

#endif
```

- [ ] **Step 4: Run the tests to verify they pass**

```sh
pio test -e native -f test_webui_gate
pio test -e native
```

Expected: 6/6 for the filtered run; **34/34** overall (28 baseline + 6). If the total is not 34, stop and report the actual number — a changed baseline means something else broke.

- [ ] **Step 5: Commit**

```sh
git add src/core/wifi/webui_gate.h test/test_webui_gate/test_webui_gate.cpp
git commit -F - <<'EOF'
test: add a pure decision module for the webui start path

`webui` reports success whether or not an HTTP server started, and the decision
it should be making has nowhere to live that can be tested: the gates read the
heap and touch WiFi, so [env:native] cannot reach them.

Extract the parts that are pure — is this DMA block sufficient, is this socket
listening, and the one-line report — so they can be tested off-device the way
crash_report.h is. The threshold cases are pinned to ISSUE-12's measured runs,
including the run that cleared 15,360 and still failed, so a later change cannot
quietly paper over it by raising the number.
EOF
```

---

### Task 2: Gates A and B — `wifiConnectMenu`'s AP case and `_setupAP()`

**Files:**
- Modify: `src/core/wifi/wifi_common.cpp:142-150` (`_setupAP`), `:182-185` (`WIFI_AP` case), `:189-192` (`WIFI_STA` case, one argument)

**Interfaces:**
- Consumes: `radioHasMemForWifi()`, `radioLargestDmaBlock()`, `RADIO_WIFI_MIN_DMA_BLOCK` — all from `core/radio_mem.h`, already included at `wifi_common.cpp:5`.
- Produces: `_setupAP()` and `wifiConnectMenu(WIFI_AP)` now return `false` on a failed or refused AP bring-up. Signatures unchanged (`wifi_common.h:21,73` already declare `bool`).

**No host test is possible.** Both functions call `WiFi.*`; `[env:native]` sets `test_build_src = no`. Verification is the build plus Task 6's hardware run.

- [ ] **Step 1: Make `_setupAP()` honour `softAP()`'s return**

Replace `wifi_common.cpp:142-150` with:

```cpp
bool _setupAP() {
    IPAddress AP_GATEWAY(172, 0, 0, 1);
    WiFi.softAPConfig(AP_GATEWAY, AP_GATEWAY, IPAddress(255, 255, 255, 0));
    // softAP()'s return was discarded here and wifiConnected set regardless, so a
    // failed AP left the whole firmware believing one existed — ISSUE-28's shape,
    // one module along, on the path `webui` actually takes.
    if (!WiFi.softAP(bruceConfig.wifiAp.ssid, bruceConfig.wifiAp.pwd, 6, 0, 4, false)) {
        log_e("_setupAP: softAP failed for SSID '%s'", bruceConfig.wifiAp.ssid.c_str());
        return false;
    }
    wifiIP = WiFi.softAPIP().toString(); // update global var
    Serial.println("IP: " + wifiIP);
    wifiConnected = true;
    return true;
}
```

The existing `Serial.println("IP: …")` success line stays as-is: it is out of scope, and promoting a success line to `log_e` would misreport its level (`log_i` is compiled out — see Global Constraints).

- [ ] **Step 2: Add gate A to the `WIFI_AP` case**

Replace `wifi_common.cpp:182-185` with:

```cpp
        case WIFI_AP: // access point
            // The STA case below has refused on a low contiguous DMA block since the
            // guard was added; the AP case never did — and `webui` defaults to AP, so
            // the mode the verb actually uses was ungated at every level.
            if (!radioHasMemForWifi()) {
                log_e(
                    "wifiConnectMenu: AP refused, largest DMA block %u < %u required",
                    (unsigned)radioLargestDmaBlock(),
                    (unsigned)RADIO_WIFI_MIN_DMA_BLOCK
                );
                displayError("Low RAM: free BLE/SD first", false);
                return false;
            }
            WiFi.mode(WIFI_AP);
            return _setupAP();
            break;
```

`displayError(..., false)`, not `true` — see Global Constraints.

- [ ] **Step 3: Stop the STA gate wedging the serial task**

At `wifi_common.cpp:189-192` change the one argument:

```cpp
            if (!radioHasMemForWifi()) {
                // false, not true: `webui -noAp` reaches this from the serial task,
                // where waitKeyPress holds the CLI with no BLE dismissal available.
                displayError("Low RAM: free BLE/SD first", false);
                return false;
            }
```

**This is a deliberate one-argument widening beyond the spec, and a reviewer may reject it.** Rationale: `startWebUi` calls `wifiConnectMenu(WIFI_STA)` at `webInterface.cpp:756`, so `webui -noAp` can already reach a blocking `displayError` from the serial task — the exact defect the spec forbids in new code, on a path RC3 owns. Cost: an on-device operator no longer gets a "press any key" acknowledgement on this banner. If rejected, revert this step only; Tasks 3–7 are unaffected.

- [ ] **Step 4: Build**

```sh
pio run -e smoochiee-board 2>&1 | tail -20
```

Expected: SUCCESS. `log_e` needs no new include — it arrives transitively via Arduino.h, as at `webInterface.cpp:384` and throughout `evil_portal.cpp`. If the compiler disagrees, add `#include <esp32-hal-log.h>` and say so in the commit body.

Three callers change behaviour; none needs editing:
- `wifi_common.cpp:184` — the intended path.
- `wifi_commands.cpp:37` (`return _setupAP();`) — `wifi on`'s AP fallback result becomes truthful; it claimed success unconditionally before.
- `attack_commands.cpp:174` — still discards the return. Out of scope, recorded in the spec.

- [ ] **Step 5: Commit**

```sh
git add src/core/wifi/wifi_common.cpp
git commit -F - <<'EOF'
fix(wifi): refuse and report a failed AP bring-up instead of claiming one

_setupAP() discarded WiFi.softAP()'s return and set wifiConnected = true
regardless, so a failed AP left every caller believing one existed — ISSUE-28's
defect verbatim, one module along. And wifiConnectMenu's WIFI_AP case had no
memory gate at all, while all three radioHasMemForWifi() sites guarded STA
paths; `webui` defaults to AP, so the mode the verb actually uses was ungated.

The STA gate's displayError also waited on a key press, which holds the serial
task with no BLE dismissal available whenever `webui -noAp` reaches it.
EOF
```

---

### Task 3: Gates C and D in `startWebUi()`, plus the reporting sink

**Files:**
- Modify: `src/core/wifi/webInterface.cpp` (includes; new `reportWebUiStart()`; `startWebUi` at `:752-802`)
- Modify: `src/core/wifi/webInterface.h:28`
- Modify: `src/core/serial_commands/wifi_commands.cpp:79-81`

**Interfaces:**
- Consumes: Task 1's `WebUiStartReport`, `WebUiStartResult`, `webUiDmaSufficient()`, `webUiListening()`, `formatWebUiStartReport()`. Task 2's meaningful `wifiConnectMenu()` return.
- Produces: `bool startWebUi(bool mode_ap = false, bool background = false)`. Task 5 adds a third parameter.

- [ ] **Step 1: Add the two includes**

After `webInterface.cpp:12` (`#include "core/wifi/ws_events.h"`), add:

```cpp
#include "core/radio_mem.h"
#include "core/wifi/webui_gate.h"
```

Already present and needed: `core/display.h` (`displayError`), `core/ram_profile.h` (`RAM_LOGF`), `core/wifi/ws_events.h` (`pushWsLog`), `<globals.h>` (`serialDevice`, declared `extern` at `globals.h:74`).

- [ ] **Step 2: Add the reporting helper**

Insert immediately above the `startWebUi` doc comment at `webInterface.cpp:748`:

```cpp
// Every WebUI start outcome goes through here. Four destinations because no single
// one reaches everybody on this board: `Serial` is the native USB-CDC port that
// nothing is attached to (ISSUE-22), the CLI reply is invisible to anyone watching
// the console, and the console is invisible to the app.
static void reportWebUiStart(const WebUiStartReport &r) {
    const String line = String(formatWebUiStartReport(r).c_str());
    const bool ok = r.result == WebUiStartResult::Started;

    // log_e for failures because CORE_DEBUG_LEVEL=1 compiles out every level below
    // ERROR; RAM_LOGF for the success line, which reaches the same console via the
    // UART0 mirror without misreporting itself as an error.
    if (ok) RAM_LOGF("%s", line.c_str());
    else log_e("%s", line.c_str());

    pushWsLog(line, ok ? "info" : "err");
    if (serialDevice) serialDevice->println(line);
    // false, never true: waitKeyPress spins on check(AnyKeyPress) and would hold the
    // serial task here with no BLE dismissal available.
    if (!ok) displayError(line, false);
}
```

- [ ] **Step 3: Rewrite `startWebUi()`**

Replace `webInterface.cpp:752-778` (from `void startWebUi(` through the closing brace of the `if (!server)` block) with:

```cpp
bool startWebUi(bool mode_ap, bool background) {
    WebUiStartReport report{};
    report.required = RADIO_WIFI_MIN_DMA_BLOCK;
    report.apMode = mode_ap;

    bool keepWifiConnected = false;
    if (WiFi.status() != WL_CONNECTED) {
        // The return was discarded here, so neither the STA memory gate nor a failed
        // softAP() could reach this caller and the WebUI was built on top of an
        // interface that might not exist.
        if (!wifiConnectMenu(mode_ap ? WIFI_AP : WIFI_STA)) {
            report.result = WebUiStartResult::WifiBringUpFailed;
            report.dmaBlock = radioLargestDmaBlock();
            reportWebUiStart(report);
            return false;
        }
    } else {
        keepWifiConnected = true;
    }

    // configure web server

    if (!server) {
        // Clear this vector to free stack memory
        options.clear();

        Serial.println("Configuring Webserver ...");
        RAM_LOG("webui pre-alloc");

        // Sampled here rather than at entry: this is the point ISSUE-12's
        // `webui pre-alloc` figures were taken, with the AP already up, so the gate
        // is directly comparable to the register's table. At function entry the
        // block is still pre-AP and was ample in both failing runs.
        report.dmaBlock = radioLargestDmaBlock();
        if (!webUiDmaSufficient(report.dmaBlock, report.required)) {
            report.result = WebUiStartResult::RefusedLowDmaPreAlloc;
            reportWebUiStart(report);
            // Refusing must not leave an AP we raised on air: ISSUE-31 and ISSUE-39
            // are both that mistake, and `free` failing to return to its idle
            // plateau was the whole tell.
            if (!keepWifiConnected) wifiDisconnect();
            return false;
        }

        if (psramFound()) server = (AsyncWebServer *)ps_malloc(sizeof(AsyncWebServer));
        else server = (AsyncWebServer *)malloc(sizeof(AsyncWebServer));

        new (server) AsyncWebServer(default_webserverporthttp);

        configureWebServer();
        RAM_LOG("webui post-configure");

        // configureWebServer() ends in server->begin(). A failed begin() returns
        // early leaving _pcb null, so state() reads CLOSED — the one exact signal
        // that port 80 is not listening, as against a heap figure that merely
        // correlates with it.
        report.tcpState = (uint8_t)server->state();
        if (!webUiListening(report.tcpState)) {
            report.result = WebUiStartResult::FailedNotListening;
            report.dmaBlock = radioLargestDmaBlock();
            reportWebUiStart(report);
            stopWebUi();
            if (!keepWifiConnected) wifiDisconnect();
            return false;
        }

        // Only now. Set unconditionally before, this told display.cpp:985 and
        // loopOptionsWebUi() that a server existed when none did.
        isWebUIActive = true;
    }

    report.result = WebUiStartResult::Started;
    report.dmaBlock = radioLargestDmaBlock();
    report.tcpState = (uint8_t)server->state();
    reportWebUiStart(report);
```

Leave `:779` onward (`tft.setLogging();` through the `#endif`) unchanged, then change the function's fall-through end to `return true;`. The `#ifdef HAS_SCREEN` block contains an early `if (background) return;` — change that to `return true;`, and add `return true;` after the `#endif`.

The resulting tail:

```cpp
    tft.setLogging();
    drawWebUiScreen(mode_ap);
#ifdef HAS_SCREEN // Headless always run in the background!
    if (background) return true;
    while (!check(EscPress)) {
        // nothing here, just to hold the screen until the server is on.
        vTaskDelay(pdMS_TO_TICKS(70));
    }

    bool closeServer = false;

    options.clear();
    options.emplace_back("Run in background", []() {});
    options.emplace_back("Exit", [&closeServer]() { closeServer = true; });

    loopOptions(options);

    if (closeServer) {
        stopWebUi();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!keepWifiConnected) { wifiDisconnect(); }
    }
#endif
    return true;
}
```

- [ ] **Step 4: Update the declaration**

`webInterface.h:28`:

```cpp
bool startWebUi(bool mode_ap = false, bool background = false);
```

The three `lambdaHelper(startWebUi, …)` call sites (`webInterface.cpp:100,107,108`) and `startup_app.cpp:44` need no change: `lambdaHelper` is `return [=]() { (void)callback(args...); };` (`globals.h:178-181`), which discards any return type.

- [ ] **Step 5: Make the verb return the outcome**

`wifi_commands.cpp:79-81`:

```cpp
    // Returned, not discarded: this is the one attack-adjacent verb whose
    // `[CLI] Result:` can now mean something, since startWebUi knows whether port
    // 80 is listening.
    return startWebUi(!noAp, background); // without bg: quits when check(EscPress)
}
```

Leave the two `serialDevice->println` lines above it (`:77-78`) alone. "Starting Web UI AP" is a statement of intent, and `reportWebUiStart` follows it with the outcome.

- [ ] **Step 6: Build and re-run host tests**

```sh
pio run -e smoochiee-board 2>&1 | tail -20
pio test -e native
```

Expected: build SUCCESS, tests **34/34**. If `startWebUi`'s new `bool` breaks a call site the plan did not name, report the site rather than working around it.

- [ ] **Step 7: Commit**

```sh
git add src/core/wifi/webInterface.cpp src/core/wifi/webInterface.h \
        src/core/serial_commands/wifi_commands.cpp
git commit -F - <<'EOF'
fix(wifi): gate the webui start on memory, and verify port 80 is listening

From a boot where a single `js` run had taken ~18 KB, `webui -bg` printed AP /
Press ESC to quit and started nothing: AsyncTCP failed to start its task with
1,235 bytes free, the AP beaconed but accepted no station, and BLE replies
truncated to 1 byte. The verb reported success either way, and the only in-band
signal of failure was that nothing answered on port 80.

startWebUi() had no memory gate, set isWebUIActive = true unconditionally, and
returned void so its caller could not report an outcome. It had no failure
detection at all — :767 is a progress line, not the failure report the register
implied.

Two decision points now: a DMA gate where RAM_LOG("webui pre-alloc") already
samples, so its threshold is comparable to ISSUE-12's table, and an exact
state() == LISTEN check after begin(), which is a fact about the socket rather
than an inference from free heap. Both unwind an AP they raised, because a
refusal that leaks 63 KB is ISSUE-39 again. isWebUIActive moves behind the
second.

Reporting goes to four places because none of them reaches everybody here:
log_e for the console, an event-stream frame for the app, the CLI reply so the
verb says why, and a non-blocking red stripe for the operator.
EOF
```

---

### Task 4: `radio_mem.h`'s diagnostics reach the console

**Files:**
- Modify: `src/core/radio_mem.h:48`, `:62`, `:67-68`

**Interfaces:**
- Consumes: nothing new. Produces: no signature change.

- [ ] **Step 1: Convert the three diagnostics**

At `radio_mem.h:48`:

```cpp
    // log_e, not Serial: `Serial` here is the native USB-CDC port while the bench
    // reads the UART bridge (see ram_profile.cpp), so these three lines were
    // unobservable by construction. That is why ISSUE-19's AP-teardown attribution
    // is stuck at SUSPECTED — the missing log line was never evidence either way.
    log_e("[RAM] Low contiguous DMA memory for BLE, attempting to free WiFi...");
```

At `:62`:

```cpp
        log_e("[RAM] WiFi freed, DMA block: %u bytes", (unsigned)radioLargestDmaBlock());
```

At `:67-68`:

```cpp
    log_e(
        "[RAM] Still only %u bytes DMA block, minimum %u needed",
        (unsigned)radioLargestDmaBlock(),
        (unsigned)RADIO_BLE_MIN_DMA_BLOCK
    );
```

Note the `%d` → `%u` change with explicit casts: both figures are `size_t`, so the originals were passing a 32-bit unsigned through a signed conversion.

- [ ] **Step 2: Build**

```sh
pio run -e smoochiee-board 2>&1 | tail -20
```

Expected: SUCCESS. `radio_mem.h` includes `<WiFi.h>` (`:21`), which pulls Arduino.h and with it `log_e`.

- [ ] **Step 3: Commit**

```sh
git add src/core/radio_mem.h
git commit -F - <<'EOF'
fix(radio): send the BLE memory guard's teardown decisions to the console

radioHasMemForBle() tears WiFi down when the contiguous DMA block is too small
and logged that decision to Serial, which on this board is the native USB-CDC
port while the bench reads the UART bridge. So when blesniffer destroyed a live
AP, neither of the guard's lines appeared in the capture — and ISSUE-19 recorded
that absence as weak evidence, when it was in fact no evidence: the lines were
never addressed to the console at all.

log_e reaches it. The next repro can settle the attribution instead of
suspecting it. Also fixes the size_t figures being printed through %d.
EOF
```

---

### Task 5: `webui -selftest`, so gate D is provable

**Files:**
- Modify: `src/core/serial_commands/wifi_commands.cpp:70-81` (`webuiCallback`), `:130-133` (flag args)
- Modify: `src/core/wifi/webInterface.h:28`, `src/core/wifi/webInterface.cpp` (gate D)

**Interfaces:**
- Consumes: Task 3's `bool startWebUi(bool, bool)`.
- Produces: `bool startWebUi(bool mode_ap = false, bool background = false, bool selftest = false)`.

**Why this ships permanently.** ISSUE-28's `beginAP()` failure branch is correct and still **UNVERIFIED**, because nothing can reach it through the CLI. Gates A and C stand in front of gate D, so it risks the same fate. A temporary forcing `#define` was rejected in the spec: the build is reproducible per *source*, so removing the flag changes the ELF hash and the evidence would attach to a build that never shipped. `crashlog -selftest` is the precedent.

- [ ] **Step 1: Register the flag**

`wifi_commands.cpp:130-133`, after `webuiCmd.addFlagArg("bg");`:

```cpp
    webuiCmd.addFlagArg("selftest");
```

- [ ] **Step 2: Thread it through the callback**

In `webuiCallback`, after `bool background = cmd.getArgument("bg").isSet();` (`:72`):

```cpp
    bool selftest = cmd.getArgument("selftest").isSet();
```

and change the call at `:79`:

```cpp
    return startWebUi(!noAp, background, selftest);
```

- [ ] **Step 3: Update the declaration**

`webInterface.h:28`:

```cpp
bool startWebUi(bool mode_ap = false, bool background = false, bool selftest = false);
```

- [ ] **Step 4: Have gate D honour it**

In `webInterface.cpp`, change the signature to `bool startWebUi(bool mode_ap, bool background, bool selftest)` and replace gate D's single read:

```cpp
        // -selftest forces the branch below without stubbing begin(): the server is
        // genuinely started and genuinely unwound, which is the half that has to be
        // proven. ISSUE-28's beginAP() guard is still UNVERIFIED because nothing can
        // reach it from the CLI; this exists so gate D does not join it.
        report.tcpState = selftest ? (uint8_t)0 : (uint8_t)server->state();
```

- [ ] **Step 5: Build**

```sh
pio run -e smoochiee-board 2>&1 | tail -20
pio test -e native
```

Expected: build SUCCESS, tests 34/34.

- [ ] **Step 6: Commit**

```sh
git add src/core/serial_commands/wifi_commands.cpp src/core/wifi/webInterface.h \
        src/core/wifi/webInterface.cpp
git commit -F - <<'EOF'
feat(cli): add webui -selftest so the not-listening branch can be exercised

The memory gates sit in front of the state() == LISTEN check, so the conditions
that produced "begin(): failed to start task" may never reach it again. That is
how ISSUE-28's beginAP() guard ended up correct and permanently UNVERIFIED:
nothing can reach it through the CLI.

-selftest substitutes a non-LISTEN state for the real read. begin() is not
stubbed, so the server really starts and the unwind really runs — freeing a live
AsyncWebServer being the dangerous half. It ships rather than living behind a
temporary #define, because the build is reproducible per source and a removed
flag would leave the evidence attached to a build nobody runs.
EOF
```

---

### Task 6: Hardware verification

**Files:** none. **Requires the board attached at `/dev/ttyACM0`.** Steps 3 and 4 need an operator only if the device needs a physical reset; the dispatches themselves are remote.

**Interfaces:** consumes Tasks 1–5, shipped.

Bench tooling: `tools/ble_spike/` (`pip install bleak`). Device `1C:DB:D4:5E:D7:39`. Capture `/dev/ttyACM0` at 115200 raw throughout — that is where `log_e` lands.

- [ ] **Step 1: Flash the build under test and record its identity**

```sh
fuser -v /dev/ttyACM0                      # must show nothing holding the port
pio run -e smoochiee-board -t upload
sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9
```

Record that digest. It is simultaneously the panic handler's `ELF file SHA256` and `crashlog`'s `elf=`/`running_elf=`. Confirm over BLE that `crashlog` reports `running_elf=` equal to it, and that the coredump partition is empty.

If a `platformio` process needs killing: `pgrep -f '[p]latformio'` then kill by bare PID. **Never `pkill -f platformio`** — it kills the shell.

- [ ] **Step 2: Baseline — the working path still works**

Fresh boot, no menu navigation (ISSUE-12: navigation, not uptime, is the variable). Over BLE:

```
webui -bg
```

Expected: `webui: started mode=ap dma=<N> required=15360 tcp_state=1`, an event frame `{"type":"log","level":"info","line":"webui: started …"}`, and `[CLI] Result: TRUE`. Then confirm a laptop associates and reaches port 80 — use a static address, per ISSUE-12:

```sh
nmcli con mod <profile> ipv4.method manual ipv4.addresses 172.0.0.5/24 \
      ipv4.gateway "" ipv4.never-default yes
```

Record `dma=` — this is the post-`begin()` figure ISSUE-12 suspects predicts the outcome. Then `webui -off` and confirm `free` returns to its idle plateau.

- [ ] **Step 3: Gate C — induce the refusal**

Reboot. Then, over BLE, in this order and with no menu navigation:

```
js                 # ISSUE-17: transiently holds ~18 KB
webui -bg
```

Expected: `webui: low_dma_pre_alloc mode=ap dma=<N> required=15360 tcp_state=0` where `N < 15360`, an event frame at `level:"err"`, `[CLI] Result: FALSE`, and `[E]` on the console capture.

Then the two checks that actually prove the unwind:

```sh
nmcli device wifi list --rescan yes | grep -i BruceNet   # expect no match
```

and over BLE, `free` — expect the **idle plateau** (~81,000 free / ~31,700 largest DMA), not ~18,000. Per ISSUE-39, an AP left on air holds ~63 KB, and this is the whole tell.

If `js` does not drive the block under 15,360, say so and report the figure rather than reporting the gate untested — ISSUE-12's own numbers vary run to run.

- [ ] **Step 4: Gate D — force the not-listening branch**

Reboot. Over BLE:

```
webui -selftest -bg
```

Expected: `webui: not_listening mode=ap dma=<N> required=15360 tcp_state=0`, an `err` frame, `[CLI] Result: FALSE`, **no panic** in the console capture (this path frees a live `AsyncWebServer`, historically the most dangerous operation in this file — see `ws_events.cpp:6-14`), no AP in an `nmcli` rescan, and `free` back at the idle plateau.

- [ ] **Step 5: Close out**

```
crashlog
```

Expected: no dump, or if one exists, `match=yes` against Step 1's digest. Then confirm the tree is still clean and the flashed ELF still equals it.

- [ ] **Step 6: Commit the measurements**

No code. Record the three runs' figures verbatim in the Task 7 docs commit. Phrase every result as "no failure observed in N s", never "safe".

---

### Task 7: Documentation

**Files:**
- Modify: `docs/KNOWN_ISSUES.md`, `docs/bruce-companion-api.md`, `docs/TEST_STATUS.md`, `BRUCELINK.md`

Do this **after** Task 6 so the entries record measurements, not expectations. If Task 6 was skipped or partial, mark the affected claims **UNVERIFIED** and say why.

- [ ] **Step 1: `docs/KNOWN_ISSUES.md`**

- **ISSUE-12** — add the root cause (`startWebUi` had no gate, no failure detection, and an unconditional `isWebUIActive`), the fix with its commit and ELF hash, Task 6's measurements, and an explicit statement that **the marginal class is not fixed**: run 2 (`dma=18,420`) cleared the threshold, logged no AsyncTCP error, accepted TCP and failed on body allocation, so gate D passes it correctly. Do not claim ISSUE-12 resolved.
- **ISSUE-19** — replace the missing-log-line argument. It was **invalid by construction**, not weak: `radio_mem.h:48,62,67` wrote to the native USB-CDC port and `displayError` draws to the TFT (`display.cpp:317`). Note that Task 4 makes the next repro decisive. Leave the attribution **SUSPECTED** — this change makes it testable, it does not test it.
- **ISSUE-28** — cross-reference `_setupAP()` as a third site of the same shape, now fixed. Leave ISSUE-28's own `OPEN` status alone: its `beginAP()` branch remains unreachable via the CLI and that status is deliberate.
- **New entry** for `_setupAP()`'s discarded return, or fold it into ISSUE-28 with its own status line. Record `attack_commands.cpp:174` as still discarding it.

- [ ] **Step 2: `docs/bruce-companion-api.md`**

- Correct `:347`. "That is the only `log` source" is false — `evil_portal_bg.cpp:37,102,146,150`, `globals_js.cpp:287`, `serial_js.cpp:54` all emit `log` frames, and `:146` already uses `warn`.
- Document RC3's frames: `level:"err"` now has producers (first in the firmware), and the success frame carries the post-`begin()` DMA figure.
- Document that `webui`'s `[CLI] Result:` is now meaningful, unlike the attack verbs (ISSUE-7).
- Document `webui -selftest`.
- Fix the drifted citations at `:346` (`serialcmds.cpp:45,52,64,68` → `65,72,85,89`).
- Bump the contract to **2.5**.

- [ ] **Step 3: `BRUCELINK.md`**

- Add to §Known gotchas: **`Serial` is the native USB-CDC port on this board and reaches nothing; the bench reads the UART bridge, which carries ESP_LOG/UART0.** Cite `ram_profile.cpp:9-18`. Note `RAM_LOG` works only via its explicit GPIO43 mirror, and that `log_e` is the only compiled level (`CORE_DEBUG_LEVEL=1`, `boards/smoochiee-board/smoochiee-board.ini:21`). This is the root cause behind ISSUE-2, ISSUE-22, ISSUE-42 and ISSUE-19's stalled attribution, and it is currently only implicit.
- Add the new commit's ELF digest to the §Testing commit↔ELF table.
- Update the ISSUE-12 summary in §The one thing to understand first, which currently describes the silent-failure behaviour as current.

- [ ] **Step 4: `docs/TEST_STATUS.md`**

Update the `webui` row with Task 6's results. While there: **ISSUE-39's status still cites ELF `46d975be7d38f128` as "awaiting operator verification"** — its fix is in the current build. Correct the citation (verifying the behaviour itself still needs an operator, and needs LEFT+RIGHT **twice**, since the first press on a dimmed screen is swallowed — `interface.cpp:119-123`).

- [ ] **Step 5: Commit**

```sh
git add docs/KNOWN_ISSUES.md docs/bruce-companion-api.md docs/TEST_STATUS.md BRUCELINK.md
git commit -F - <<'EOF'
docs: record the webui start gate, and retire ISSUE-19's invalid evidence

ISSUE-12's root cause is written down with the fix and its measurements, and so
is what the fix does not cover: the marginal class where the listener starts and
a request's body allocation fails clears the threshold and passes the LISTEN
check, correctly. Not marked resolved.

ISSUE-19's "neither log line appears in the capture" argument is withdrawn. Both
lines were unobservable by construction — one wrote to the native USB-CDC port,
the other draws to the TFT — so their absence was never evidence. The
attribution stays SUSPECTED; it is now testable.

Adds the Serial-is-USB-CDC fact to the gotchas, which was implicit in a source
comment while being the root cause behind four register entries.
EOF
```

- [ ] **Step 6: Push**

```sh
git push origin main
git fetch -q origin && git rev-list --count origin/main..HEAD   # expect 0
```

---

## Self-review notes

**Spec coverage.** Every spec section maps to a task: the four decision points → Tasks 2 and 3; `isWebUIActive` behind gate D and the `bool` return → Task 3; the unwind ledger → Task 3; the pure module → Task 1; the four-destination sink → Task 3 Step 2; `radio_mem.h` → Task 4; `-selftest` → Task 5; the host and hardware test plans → Tasks 1 and 6; every documentation obligation → Task 7. The spec's *Honest limits* are carried into Task 7 Step 1 as explicit non-claims rather than being dropped.

**Two deliberate deviations from the spec**, both flagged in place:
1. The enum has **four** values, not five. The spec implied separate slugs for gate A and gate B; `startWebUi` cannot tell them apart from a bare `bool`, so reporting either would be a guess. One `wifi_bringup_failed` slug, with each gate logging its own reason.
2. The success line uses **`RAM_LOGF`**, not `log_e`. The spec's sink table lists `log_e` for all outcomes; using an ERROR level for a success is a misreport, and `log_i` is compiled out. `RAM_LOGF` reaches the same console via the UART0 mirror.

One step widens scope by one argument — Task 2 Step 3, `displayError`'s `true` → `false` on the pre-existing STA gate — and is marked as independently revertable.
