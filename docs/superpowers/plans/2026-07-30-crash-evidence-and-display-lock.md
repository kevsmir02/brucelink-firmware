# Crash Evidence and Display Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read the ELF core dumps this board already writes to flash and report them over BLE, then replace the unsound `tftMutex` patch with an explicitly paired display lock at the two sites that actually collide.

**Architecture:** RC4 adds a pure, host-tested formatter (`crash_report.h`) plus a `crashlog` verb that feeds it from `esp_core_dump_get_summary()`. RC1-A reverts the in-library semaphore calls from `2d9422ea`/`d2fc9d94`, introduces `tft_lock.{h,cpp}` — a plain recursive mutex with no coupling to TFT_eSPI's internal flags — and brackets three points: both `serialCli.parse()` call sites (wait forever) and the main loop's 30 s `drawStatusBar()` repaint (bounded 20 ms, skip on timeout).

**Tech Stack:** C++17, PlatformIO, Arduino-ESP32 (pioarduino 55.03.39), FreeRTOS, ESP-IDF core dump API, SimpleCLI, Unity (host tests via `[env:native]`).

**Spec:** `docs/superpowers/specs/2026-07-30-crash-evidence-and-display-lock-design.md`

## Global Constraints

- **Build:** `pio run -e smoochiee-board` must exit 0 after every task.
- **Host tests:** `pio test -e native` must stay green (21 cases at baseline, plus the new ones).
- **`[env:native]` builds pure logic only** (`test_build_src = no`, `build_flags = -std=gnu++17 -I src`). Anything touching Arduino, FreeRTOS, or ESP-IDF cannot be tested there.
- **Never write user-facing output to `Serial`.** `Serial` is the native USB-CDC port and reaches nothing on this board (ISSUE-22). Use `serialDevice->` for anything the app must see, `log_e()` for anything a console must see. This is what made ISSUE-42 a defect.
- **Comments explain *why*, never *what*.** Match the surrounding density. Do not add narration.
- **Formatting:** `.clang-format` (LLVM base, 4-space indent, 110 col). Run it on every file you touch under `src/`. **Do not** reformat `lib/TFT_eSPI/` — it is vendored upstream code with its own 2-space style; match the file you are in.
- **Commit style:** `type(scope): imperative summary`, lowercase. The body explains the *symptom* that motivated the change. Types in use: `feat`, `fix`, `docs`, `tools`, `test`, `refactor`.
- **No AI attribution in commits.** Never a `Co-Authored-By:` line for an assistant, never a "Generated with" line.
- **Baseline:** HEAD `2cb13065`, branch `main`, tree clean. The user has standing consent to commit on `main` in this repo.
- **Do not touch `lib/HAL/display/ardgfx.cpp`.** It declares its own unrelated file-static `SemaphoreHandle_t tftMutex` and will appear in any `grep tftMutex`. It is a different display HAL, not used by this board.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `src/core/crash_report.h` | Pure formatting of a crash summary and reset reason. No Arduino/FreeRTOS/IDF. Host-tested. |
| `src/core/serial_commands/crash_commands.h` | Declares `createCrashCommands()` and `reportBootCrashState()`. |
| `src/core/serial_commands/crash_commands.cpp` | The `crashlog` verb; reads the IDF core dump API and feeds `crash_report.h`. |
| `src/core/tft_lock.h` | Display-lock interface and the two timeout constants. |
| `src/core/tft_lock.cpp` | Recursive mutex, timeout counter. |
| `test/test_crash_report/test_crash_report.cpp` | Unity tests for the pure formatter. |

**Modified:**

| File | Change |
|---|---|
| `src/core/serial_commands/cli.cpp:1-18, 38-61` | Include and register the crash commands. |
| `src/main.cpp:474` | `tftLockInit()` after `tft.init()`. |
| `src/main.cpp:588-593` | `reportBootCrashState()` before `RAM_LOG("setup-end")`. |
| `lib/TFT_eSPI/TFT_eSPI.cpp:72, 80-94, 96, 99, 136, 139, 110, 113, 153, 156, 165, 170, 194, 204, 646` | Revert all semaphore calls and the global. |
| `lib/TFT_eSPI/Extensions/Touch.cpp:20-29, 31, 37, 58, 65` | Revert the same. |
| `src/core/serialcmds.cpp:67, 87` | Bracket both `parse()` sites via one shared helper. |
| `src/core/display.cpp:592-595` | Bracket the 30 s `drawStatusBar()` with a bounded wait. |
| `src/core/serial_commands/util_commands.cpp:105-130` | Add `tftdrop=` to the `free` verb's output. |
| `docs/KNOWN_ISSUES.md`, `docs/TEST_STATUS.md`, `docs/bruce-companion-api.md`, `BRUCELINK.md` | Corrections (Tasks 9 and 10). |

---

## Task 1: Pure crash-summary formatter

**Files:**
- Create: `src/core/crash_report.h`
- Test: `test/test_crash_report/test_crash_report.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct CrashSummaryView`; `std::string formatCrashSummary(const CrashSummaryView &)`; `const char *resetReasonName(int)`. Task 2 is the only consumer.

- [x] **Step 1: Write the failing test**

Create `test/test_crash_report/test_crash_report.cpp`:

```cpp
#include "core/crash_report.h"
#include <string.h>
#include <unity.h>

// The formatter is the only part of the crash path that can be tested off-device:
// esp_core_dump_get_summary() needs a real panic and real flash.

static const uint32_t kBt[4] = {0x420a1c34u, 0x420a0f88u, 0x4037d1a0u, 0x4037c904u};

static CrashSummaryView baseView() {
    CrashSummaryView v;
    v.taskName = "main";
    v.excPc = 0x420a1c34u;
    v.bt = kBt;
    v.depth = 4;
    v.btCapacity = 16;
    v.corrupted = false;
    v.elfSha256 = "2841bf2b5";
    return v;
}

void test_renders_header_fields() {
    std::string s = formatCrashSummary(baseView());
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "task=main"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "pc=0x420a1c34"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "elf=2841bf2b5"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=4"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "corrupted=no"));
}

void test_renders_backtrace_lowercase_in_order() {
    std::string s = formatCrashSummary(baseView());
    TEST_ASSERT_NOT_NULL(
        strstr(s.c_str(), "bt=0x420a1c34 0x420a0f88 0x4037d1a0 0x4037c904")
    );
}

// A dump with depth 0 and a null bt must not be dereferenced. This is the
// shape a corrupted dump arrives in.
void test_zero_depth_emits_no_backtrace_line() {
    CrashSummaryView v = baseView();
    v.depth = 0;
    v.bt = nullptr;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NULL(strstr(s.c_str(), "bt="));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=0"));
}

// depth is read straight out of the dump, so a corrupt value must not walk
// off the end of bt[].
void test_depth_clamps_to_capacity() {
    CrashSummaryView v = baseView();
    v.depth = 99;
    v.btCapacity = 4;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=4"));
    TEST_ASSERT_NULL(strstr(s.c_str(), "0x00000000"));
}

void test_corrupted_flag_renders_yes() {
    CrashSummaryView v = baseView();
    v.corrupted = true;
    TEST_ASSERT_NOT_NULL(strstr(formatCrashSummary(v).c_str(), "corrupted=yes"));
}

void test_missing_strings_render_placeholder_not_garbage() {
    CrashSummaryView v = baseView();
    v.taskName = "";
    v.elfSha256 = nullptr;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "task=?"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "elf=?"));
}

void test_reset_reason_names() {
    TEST_ASSERT_EQUAL_STRING("panic", resetReasonName(4));
    TEST_ASSERT_EQUAL_STRING("poweron", resetReasonName(1));
    TEST_ASSERT_EQUAL_STRING("task_wdt", resetReasonName(6));
    TEST_ASSERT_EQUAL_STRING("brownout", resetReasonName(9));
    TEST_ASSERT_EQUAL_STRING("unknown", resetReasonName(255));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_renders_header_fields);
    RUN_TEST(test_renders_backtrace_lowercase_in_order);
    RUN_TEST(test_zero_depth_emits_no_backtrace_line);
    RUN_TEST(test_depth_clamps_to_capacity);
    RUN_TEST(test_corrupted_flag_renders_yes);
    RUN_TEST(test_missing_strings_render_placeholder_not_garbage);
    RUN_TEST(test_reset_reason_names);
    return UNITY_END();
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `pio test -e native -f test_crash_report`
Expected: FAIL — `core/crash_report.h: No such file or directory`.

- [x] **Step 3: Write the implementation**

Create `src/core/crash_report.h`:

```cpp
#ifndef __CRASH_REPORT_H__
#define __CRASH_REPORT_H__

#include <stdint.h>
#include <stdio.h>
#include <string>

// Deliberately free of Arduino, FreeRTOS and ESP-IDF so it builds under
// [env:native]. esp_core_dump_summary_t cannot come in here; crash_commands.cpp
// copies the fields it needs into the view below.

struct CrashSummaryView {
    const char *taskName;  // may be null or empty
    uint32_t excPc;
    const uint32_t *bt; // may be null, in which case depth is treated as 0
    uint32_t depth;     // read from the dump, so clamped rather than trusted
    uint32_t btCapacity;
    bool corrupted;
    const char *elfSha256; // NUL-terminated ASCII, may be null or empty
};

// Values of esp_reset_reason_t, spelled as ints so this stays IDF-free.
static inline const char *resetReasonName(int reason) {
    switch (reason) {
        case 1: return "poweron";
        case 2: return "external";
        case 3: return "sw_restart";
        case 4: return "panic";
        case 5: return "int_wdt";
        case 6: return "task_wdt";
        case 7: return "other_wdt";
        case 8: return "deepsleep";
        case 9: return "brownout";
        case 10: return "sdio";
        default: return "unknown";
    }
}

// One field per token and at most two lines, because this is read over BLE where
// a reply can truncate (ISSUE-16) and a half-received line must still be useful.
static inline std::string formatCrashSummary(const CrashSummaryView &v) {
    const char *task = (v.taskName && v.taskName[0]) ? v.taskName : "?";
    const char *elf = (v.elfSha256 && v.elfSha256[0]) ? v.elfSha256 : "?";

    uint32_t depth = v.depth;
    if (depth > v.btCapacity) depth = v.btCapacity;
    if (v.bt == nullptr) depth = 0;

    char buf[64];
    std::string out = "crash: task=";
    out += task;
    snprintf(buf, sizeof(buf), " pc=0x%08x", (unsigned)v.excPc);
    out += buf;
    out += " elf=";
    out += elf;
    snprintf(buf, sizeof(buf), " depth=%u corrupted=", (unsigned)depth);
    out += buf;
    out += v.corrupted ? "yes" : "no";

    if (depth > 0) {
        out += "\r\ncrash: bt=";
        for (uint32_t i = 0; i < depth; i++) {
            if (i) out += ' ';
            snprintf(buf, sizeof(buf), "0x%08x", (unsigned)v.bt[i]);
            out += buf;
        }
    }
    return out;
}

#endif
```

- [x] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native`
Expected: PASS. 28 cases total (21 baseline + 7 new).

- [x] **Step 5: Commit**

```bash
git add src/core/crash_report.h test/test_crash_report/test_crash_report.cpp
git commit -m "test: add a pure formatter for ESP-IDF crash summaries

The board has been writing ELF core dumps to a 64 KB coredump partition since
the fork began -- ENABLE_TO_FLASH, DATA_FORMAT_ELF and CHECK_BOOT are all set in
the framework sdkconfig and libespcoredump.a is linked -- and nothing has ever
read one. ISSUE-11 is filed as an unexplained reboot with no console capture.

Formatting is the only part of that path that can be tested off-device, so it
goes in a pure header the native environment can build. depth is read straight
out of the dump and clamped rather than trusted: a corrupted dump is exactly the
case this has to survive."
```

---

## Task 2: `crashlog` verb and boot reset-reason line

**Files:**
- Create: `src/core/serial_commands/crash_commands.h`, `src/core/serial_commands/crash_commands.cpp`
- Modify: `src/core/serial_commands/cli.cpp:1-18` (include), `src/core/serial_commands/cli.cpp:38-47` (register), `src/main.cpp:588-593`

**Interfaces:**
- Consumes: `formatCrashSummary()`, `resetReasonName()`, `CrashSummaryView` from Task 1. `serialDevice` from `<globals.h>` (declared `include/globals.h:74`).
- Produces: `void createCrashCommands(SimpleCLI *cli)`; `void reportBootCrashState()`.

- [x] **Step 1: Write the header**

Create `src/core/serial_commands/crash_commands.h`:

```cpp
#ifndef __CRASH_COMMANDS_H__
#define __CRASH_COMMANDS_H__

#include <SimpleCLI.h>

void createCrashCommands(SimpleCLI *cli);

// Emits one log_e line naming this boot's reset reason, and whether a stored
// core dump is waiting. log_e, not Serial: UART0 is the only console that is
// actually attached on this board (ISSUE-22).
void reportBootCrashState();

#endif
```

- [x] **Step 2: Write the implementation**

Create `src/core/serial_commands/crash_commands.cpp`:

```cpp
#include "crash_commands.h"
#include "core/crash_report.h"
#include <SimpleCLI.h>
#include <esp_system.h>
#include <globals.h>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>

static bool haveStoredDump() {
    size_t addr = 0, size = 0;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0;
}

static uint32_t crashlogCallback(cmd *c) {
    Command cmd(c);

    serialDevice->printf(
        "crash: reset_reason=%s(%d)\r\n", resetReasonName((int)esp_reset_reason()), (int)esp_reset_reason()
    );

    if (cmd.getArgument("clear").isSet()) {
        esp_err_t err = esp_core_dump_image_erase();
        // Both ternary branches are String: mixing a literal with a String would
        // make the common type depend on a user-defined conversion.
        serialDevice->println(
            err == ESP_OK ? String("crash: cleared")
                          : String("crash: erase failed err=") + (int)err
        );
        return err == ESP_OK;
    }

    if (!haveStoredDump()) {
        serialDevice->println("crash: none stored");
        return true;
    }

    esp_core_dump_summary_t summary;
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK) {
        // A dump that exists but will not parse is itself the finding: say so
        // rather than reporting "none stored" and losing the distinction.
        serialDevice->println(String("crash: stored dump unreadable err=") + (int)err);
        return false;
    }

    CrashSummaryView view;
    view.taskName = summary.exc_task;
    view.excPc = summary.exc_pc;
    view.bt = summary.exc_bt_info.bt;
    view.depth = summary.exc_bt_info.depth;
    view.btCapacity = sizeof(summary.exc_bt_info.bt) / sizeof(summary.exc_bt_info.bt[0]);
    view.corrupted = summary.exc_bt_info.corrupted;
    view.elfSha256 = (const char *)summary.app_elf_sha256;

    // SerialDevice::println takes a String; wrap explicitly rather than leaning on
    // the implicit const char* conversion against its integer overloads.
    serialDevice->println(String(formatCrashSummary(view).c_str()));
    return true;
}

void reportBootCrashState() {
    esp_reset_reason_t reason = esp_reset_reason();
    log_e("[BOOT] reset_reason=%s(%d) coredump=%s",
          resetReasonName((int)reason),
          (int)reason,
          haveStoredDump() ? "stored" : "none");
}

void createCrashCommands(SimpleCLI *cli) {
    Command crashCmd = cli->addCommand("crashlog", crashlogCallback);
    crashCmd.addFlagArg("clear");
}

#else // core dump not compiled in

static uint32_t crashlogCallback(cmd *c) {
    // An absent capability must say so. A verb that silently disappears on
    // another board profile is the ISSUE-4 mistake in a new place.
    serialDevice->printf(
        "crash: reset_reason=%s(%d)\r\n", resetReasonName((int)esp_reset_reason()), (int)esp_reset_reason()
    );
    serialDevice->println("crash: core dump not compiled in for this build");
    return true;
}

void reportBootCrashState() {
    log_e("[BOOT] reset_reason=%s(%d) coredump=disabled",
          resetReasonName((int)esp_reset_reason()),
          (int)esp_reset_reason());
}

void createCrashCommands(SimpleCLI *cli) {
    Command crashCmd = cli->addCommand("crashlog", crashlogCallback);
    crashCmd.addFlagArg("clear");
}

#endif
```

- [x] **Step 3: Register the verb**

In `src/core/serial_commands/cli.cpp`, add the include alongside the others (keep the list alphabetical among its neighbours — insert after `#include "core/sd_functions.h"`):

```cpp
#include "crash_commands.h"
```

Then in `SerialCli::setup()`, add to the unconditional block at `:38-47`, after `createCryptoCommands(&_cli);`:

```cpp
    createCrashCommands(&_cli);
```

Registered unconditionally, not behind `#ifndef LITE_VERSION`: a crash report is worth more on a stripped build, not less.

- [x] **Step 4: Call the boot hook**

In `src/main.cpp`, add the include near the other serial_commands includes, then insert the call immediately before `RAM_LOG("setup-end");` (currently `:592`), *outside* the `#if !defined(LITE_VERSION)` block that closes on the line above:

```cpp
    reportBootCrashState();

    RAM_LOG("setup-end");
```

- [x] **Step 5: Build and verify the verb registered**

Run: `pio run -e smoochiee-board`
Expected: exit 0.

Run: `pio test -e native`
Expected: PASS, 28 cases — confirms the new header did not break the host build.

- [x] **Step 6: Commit**

```bash
git add src/core/serial_commands/crash_commands.h src/core/serial_commands/crash_commands.cpp \
        src/core/serial_commands/cli.cpp src/main.cpp
git commit -m "feat(cli): add crashlog so a panic can be read without a console

Proving anything about a crash on this board has required an operator with
usbwatch2.py already capturing /dev/ttyACM0 at the moment it happened, because
the panic backtrace appears nowhere else. ISSUE-11 is filed as an unexplained
reboot with no capture, and ISSUE-30 needed five attempts partly for the same
reason.

The dump was always there. crashlog reads it over BLE: faulting task, exception
PC, on-device Xtensa backtrace, and app_elf_sha256 -- which the SDK stores as the
same 9-character ASCII prefix the panic handler prints, so it is directly
comparable to a local build with no conversion. That is the check the decode
workflow already demands and previously had to do by hand.

Output goes to serialDevice, never Serial, or it would reach nobody on this
board -- the ISSUE-42 mistake."
```

---

## Task 3: RC4 hardware verification — ✅ DONE 2026-07-30, app elf `79fc138cd`

> **Step 2 was wrong and was replaced.** It said to compare `sha256sum firmware.elf`
> against `crashlog`'s `elf=`; those are different digests and could never match.
> `crashlog` now prints `running_elf=` from `esp_app_get_elf_sha256_str()` with a
> `match=` verdict, settled on-device. Confirmed: the panic handler's
> `ELF file SHA256: 79fc138cd` is byte-identical to it.
>
> **Step 3 was also wrong**: it said provoke the panic with `deauth`, but `deauth`
> ran 21 min clean on the fixed build, so a null result would prove nothing. Replaced
> by `crashlog -selftest`, a deliberate `abort()` with a *known* expected backtrace.
>
> **Step 4 is impossible as written**: this board is USB-powered, so unplugging it
> powers it off. Done as "no process holding the port" (`fuser`: no holders) instead.

**Files:** none. This task produces evidence, not code.

No fix is trustworthy until this passes, and every later task depends on `crashlog` being reliable.

- [x] **Step 1: Flash and confirm a clean baseline**

```bash
pio run -e smoochiee-board -t upload
```

Send `crashlog` over BLE (`.venv/bin/python tools/ble_spike/bcli.py "crashlog"`).
Expected: `crash: reset_reason=poweron(1)` or `sw_restart(3)`, then `crash: none stored`.

If a dump *is* stored, it is from a real earlier panic. Record it, then `crashlog -clear` before continuing.

- [x] **Step 2: Record the local ELF prefix**

```bash
sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9
```

Note the value. `crashlog`'s `elf=` must match it in step 5, or the decode is fiction.

- [x] **Step 3: Provoke a panic, with the console captured as a control**

Terminal 1: `.venv/bin/python tools/ble_spike/usbwatch2.py` — **only ever run one instance**; two silently split the stream and both look empty.

Terminal 2: dispatch `deauth` over BLE and wait. ISSUE-1 records 70–130 s to the assert; allow 180 s.

Expected in terminal 1: `assert failed: xTaskPriorityDisinherit`, a `Backtrace:` line, and an `ELF file SHA256:` line. Save the capture.

- [x] **Step 4: Unplug the console**

Physically disconnect USB, or stop the capture and close the port. This is the condition the feature exists for.

- [x] **Step 5: Read the dump over BLE**

```bash
.venv/bin/python tools/ble_spike/bcli.py "crashlog"
```

Expected: `reset_reason=panic(4)`, a `task=`, a `pc=`, an `elf=` equal to step 2's value, and a `bt=` line.

- [x] **Step 6: Cross-check the decode against the control capture**

```bash
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/smoochiee-board/firmware.elf <the bt= addresses>
```

Expected: the same call path the terminal-1 capture showed. **This agreement is what makes `crashlog` trustworthy for every later use.** If they disagree, stop and investigate before proceeding to Task 4.

- [x] **Step 7: Verify erase**

`crashlog -clear` → `crash: cleared`, then `crashlog` → `crash: none stored`.

- [x] **Step 8: Record the result**

Append to `docs/TEST_STATUS.md` under **Shippable today**: the `crashlog` row, with the device date and the ELF prefix, and note that the decode matched a console capture of the same crash. Commit:

```bash
git add docs/TEST_STATUS.md
git commit -m "docs: record crashlog verified on hardware against a console control"
```

---

---

> ## ⚠️ Tasks 4-7 WITHDRAWN 2026-07-30, before any of them ran
>
> They rest on a premise this plan got wrong. The spec called the `tftMutex`
> scheme unsound and said its invariant "holds by luck"; that is **too strong for
> this codebase**, and reverting it would have deleted evidence-backed work.
>
> **The evidence, which lives in `2d9422ea`'s commit message and in no document:**
> verified on hardware, ELF `5186685c0fdf19c2` — `deauth` ran **21 minutes** with
> no assert, no backtrace and no reset, across roughly **forty status-bar redraws
> from the main loop**, which is the exact collision. It had previously died within
> 20-70 s, twice. ISSUE-1 was left open deliberately, because a negative result
> against a race is probabilistic rather than proof.
>
> **The 32-vs-57 imbalance is real but inert.** Every unmatched give lands on
> recursion count 0, where FreeRTOS `xQueueGiveMutexRecursive` returns `pdFAIL`
> and does nothing. The harmful case needs an unmatched give while an outer
> transaction holds count 1, and that nesting does not occur here — verified:
> **zero** `startWrite()`/`endWrite()` call sites in `src/` and `boards/`;
> `TFT_eSprite` unused in `src/` so `lockTransaction` is always false
> (`Sprite.cpp:42` is its only true-setter); and the sole internal caller of a
> comment-out composite is `drawString` (5 x `drawRect` at `:5781-5793`), which
> holds no transaction anywhere in `:5584-5810`. It is a **latent trap** for future
> code, live the moment anyone adds `startWrite()` to `src/` — not a present defect.
>
> **Also corrected:** `tftMutex` is not fork work. It originates in **upstream**
> `517cec01` as a call-scoped mutex; `2d9422ea` re-scoped it to the transaction and
> extended it to the two pairs upstream left unprotected.
>
> **And ISSUE-30's mutex hypothesis is weaker than the spec claimed.** The
> imbalance runs in the safe direction — more gives than takes, so it cannot leak —
> and no take-without-give path was found. It stays a candidate, not the mechanism.
>
> **Why not build the outer lock anyway:** the library already serialises at
> *transaction* granularity, which is why the main loop got ~40 successful redraws
> during a 21-minute `deauth`. A coarse lock around whole verbs would make the main
> loop skip repaints for a verb's entire life — a regression against a working
> interleave.
>
> **Replaced by:** Task 9 (docs), which is now the real deliverable, because
> ISSUE-1, TEST_STATUS and BRUCELINK all present this as unmitigated and none of
> them record the 21 minutes. The tasks below are kept unrun for the record.

## Task 4 (WITHDRAWN — see the notice above): Revert the unsound in-library semaphore calls

**Files:**
- Modify: `lib/TFT_eSPI/TFT_eSPI.cpp`, `lib/TFT_eSPI/Extensions/Touch.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. After this task the display has no cross-task protection, which is where the register already believes it is. Tasks 5–7 restore it correctly.

**Why revert rather than repair:** the scheme assumes `begin_tft_write` and `end_tft_write` are paired. TFT_eSPI breaks that deliberately — **32 live `begin_tft_write()` calls against 57 live `end_tft_write()` calls**, with 20 sites commenting the begin out for Sprite reuse (`:2330, 2404, 2472, 2548, 2600, 2647, 2668, 2694, 2716, 2758, 2815, 2839, 2862, 2886, 3280, 3318, 3411, 3551, 3938, 5355`), and `drawArc:4133` setting `inTransaction` with no begin at all. So the trailing unconditional give runs ~25 times more than its take. FreeRTOS fails an unmatched give harmlessly but **decrements a held one**, releasing the display mid-transaction. `drawRoundRect` is one of the 20 and sits on ISSUE-1's own second backtrace.

`locked` is TFT_eSPI's single-threaded reentrancy flag and cannot express "another task owns the bus", so there is no in-place repair.

- [ ] **Step 1: Remove the two takes from `begin_tft_write`**

In `lib/TFT_eSPI/TFT_eSPI.cpp`, replace lines 95-106 with:

```cpp
inline void TFT_eSPI::begin_tft_write(void){
  if (locked) {
    locked = false; // Flag to show SPI access now unlocked
#if defined (SPI_HAS_TRANSACTION) && defined (SUPPORT_TRANSACTIONS) && !defined(TFT_PARALLEL_8_BIT) && !defined(RP2040_PIO_INTERFACE)
    spi.beginTransaction(SPISettings(SPI_FREQUENCY, MSBFIRST, TFT_SPI_MODE));
#endif
    CS_L;
    SET_BUS_WRITE_MODE;  // Some processors (e.g. ESP32) allow recycling the tx buffer when rx is not used
  }
}
```

- [ ] **Step 2: Delete the now-false explanatory comment**

Delete lines 80-94 — the block beginning `// A note on the two xSemaphoreTakeRecursive calls below`. It documents a scheme that no longer exists.

- [ ] **Step 3: Remove the takes from `begin_nin_write`**

Apply the identical change to `begin_nin_write` (was `:109-120`): drop both `xSemaphoreTakeRecursive` lines, keep everything else.

- [ ] **Step 4: Remove both gives from `end_tft_write` and `end_nin_write`**

For each, delete the `xSemaphoreGiveRecursive(tftMutex); // release the transaction-lifetime hold` line inside the `if (!locked)` block, **and** the trailing `xSemaphoreGiveRecursive(tftMutex); // per-call` line. `end_tft_write` becomes:

```cpp
inline void TFT_eSPI::end_tft_write(void){
  if(!inTransaction) {      // Flag to stop ending transaction during multiple graphics calls
    if (!locked) {          // Locked when beginTransaction has been called
      locked = true;        // Flag to show SPI access now locked
      SPI_BUSY_CHECK;       // Check send complete and clean out unused rx data
      CS_H;
      SET_BUS_READ_MODE;    // In case bus has been configured for tx only
#if defined (SPI_HAS_TRANSACTION) && defined (SUPPORT_TRANSACTIONS) && !defined(TFT_PARALLEL_8_BIT) && !defined(RP2040_PIO_INTERFACE)
      spi.endTransaction();
#endif
    }
  }
}
```

- [ ] **Step 5: Remove the takes and gives from the read pair**

`begin_tft_read` (was `:164-181`): delete the per-call take at `:165` and the transaction take at `:170`.
`end_tft_read` (was `:187-204`): delete the give at `:194` and the trailing per-call give at `:204`.

- [ ] **Step 6: Remove the touch pair**

In `lib/TFT_eSPI/Extensions/Touch.cpp` — **this file is compiled on this board**, because `boards/smoochiee-board/pins_arduino.h:77` has `#define TOUCH_CS -1` and the include guard at `TFT_eSPI.cpp:6218` is `#ifdef TOUCH_CS`, which a `-1` value still satisfies.

Delete the takes at `:31` and `:37`, the give at `:58`, and the trailing give at `:65`. Delete the comment block at `:20-29` that explains the two-level hold.

- [ ] **Step 7: Remove the global and its creation**

Delete `SemaphoreHandle_t tftMutex;` at `:72`.
Delete `tftMutex = xSemaphoreCreateRecursiveMutex();` — the first line of `TFT_eSPI::init(uint8_t tc)` at `:646`.

- [ ] **Step 8: Verify nothing references it and the build is clean**

```bash
grep -rn "tftMutex" lib/TFT_eSPI/ ; echo "exit=$?"
```
Expected: no output, `exit=1`. (`lib/HAL/display/ardgfx.cpp` will still match a repo-wide grep — that is a different, file-static mutex for another display HAL. Leave it alone.)

```bash
pio run -e smoochiee-board
```
Expected: exit 0.

- [ ] **Step 9: Commit**

```bash
git add lib/TFT_eSPI/TFT_eSPI.cpp lib/TFT_eSPI/Extensions/Touch.cpp
git commit -m "fix(tft): revert the in-library SPI mutex, which could not balance

2d9422ea and d2fc9d94 took a recursive mutex inside begin_tft_write and gave it
inside end_tft_write, on the assumption that the two are paired. TFT_eSPI breaks
that on purpose: 32 live begin_tft_write calls against 57 live end_tft_write
calls, because 20 sites comment the begin out so the Sprite class can reuse the
function, and drawArc sets inTransaction with no begin at all.

So the trailing unconditional give ran about 25 times more often than its take.
FreeRTOS fails an unmatched give harmlessly, which is why the display worked --
but it decrements a held one, which releases the bus mid-transaction and restores
the exact cross-task race ISSUE-1 describes. drawRoundRect is one of the 20 and
appears in ISSUE-1's second backtrace.

Both takes also used portMAX_DELAY, so a leaked hold blocks every other task on
the display forever. That accounts for every observation in ISSUE-30, including
why freeing memory did not unwedge it.

locked is a single-threaded reentrancy flag and cannot express cross-task
ownership, so there is nothing to repair in place. The lock moves to the two
sites that actually collide in the next commits."
```

---

## Task 5 (WITHDRAWN — see the notice above): The `tft_lock` module

**Files:**
- Create: `src/core/tft_lock.h`, `src/core/tft_lock.cpp`
- Modify: `src/main.cpp:474`

**Interfaces:**
- Consumes: nothing.
- Produces: `TFT_LOCK_WAIT_FOREVER`, `TFT_LOCK_DRAW_TIMEOUT_MS`, `void tftLockInit()`, `bool tftLock(uint32_t timeoutMs)`, `void tftUnlock()`, `uint32_t tftLockTimeoutCount()`. Tasks 6 and 7 consume these.

- [ ] **Step 1: Write the header**

Create `src/core/tft_lock.h`:

```cpp
#ifndef __TFT_LOCK_H__
#define __TFT_LOCK_H__

#include <stdint.h>

constexpr uint32_t TFT_LOCK_WAIT_FOREVER = 0xFFFFFFFFu;

// The main loop would rather drop a repaint than wait: a verb dispatched from
// the serial task owns the display for its whole life, and blocking here for
// minutes is indistinguishable from a wedged main loop (ISSUE-30).
constexpr uint32_t TFT_LOCK_DRAW_TIMEOUT_MS = 20;

// Call once, after tft.init() and before any task that draws is created.
void tftLockInit();

// Returns false only on timeout. Call tftUnlock() if and only if this
// returned true.
bool tftLock(uint32_t timeoutMs);
void tftUnlock();

// Bounded-wait failures since boot. Non-zero means contention happened and was
// handled; it is the signal that distinguishes a verb legitimately owning the
// screen from the ISSUE-30 wedge.
uint32_t tftLockTimeoutCount();

#endif
```

- [ ] **Step 2: Write the implementation**

Create `src/core/tft_lock.cpp`:

```cpp
#include "tft_lock.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Recursive because a verb takes the lock once at dispatch and the drawing it
// then does may re-enter through nested helpers.
//
// Deliberately NOT coupled to TFT_eSPI's `locked` / `inTransaction` members. The
// reverted patch tried that and could not balance: the library has 32 live
// begin_tft_write calls against 57 live end_tft_write calls, so takes and gives
// placed there can never pair. Those members stay what upstream intended --
// per-owner reentrancy state, touched only by whichever task holds this lock.
static SemaphoreHandle_t s_tftLock = nullptr;
static volatile uint32_t s_timeouts = 0;

void tftLockInit() {
    if (s_tftLock == nullptr) s_tftLock = xSemaphoreCreateRecursiveMutex();
}

bool tftLock(uint32_t timeoutMs) {
    // Before init the system is still single-tasked, so there is nothing to
    // serialise and callers must not be made to handle a failure that cannot
    // matter yet.
    if (s_tftLock == nullptr) return true;

    TickType_t ticks =
        (timeoutMs == TFT_LOCK_WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
    if (xSemaphoreTakeRecursive(s_tftLock, ticks) == pdTRUE) return true;

    s_timeouts++;
    return false;
}

void tftUnlock() {
    if (s_tftLock != nullptr) xSemaphoreGiveRecursive(s_tftLock);
}

uint32_t tftLockTimeoutCount() { return s_timeouts; }
```

- [ ] **Step 3: Initialise it at boot**

In `src/main.cpp`, add `#include "core/tft_lock.h"` with the other core includes, then immediately after `tft.init();` (currently `:474`):

```cpp
    tftLockInit();
```

It must precede the `xTaskCreate` calls at `:533` onward — no task may draw before the mutex exists.

- [ ] **Step 4: Build**

Run: `pio run -e smoochiee-board`
Expected: exit 0. Nothing takes the lock yet; this task only introduces it.

- [ ] **Step 5: Commit**

```bash
git add src/core/tft_lock.h src/core/tft_lock.cpp src/main.cpp
git commit -m "feat(tft): add an explicit display lock, uncoupled from TFT_eSPI state

The reverted in-library mutex failed because it hung off begin_tft_write and
end_tft_write, which TFT_eSPI does not pair. This one is a plain recursive mutex
with an explicit init, a bounded-wait option and a timeout counter, taken only
by call sites this fork owns.

The counter matters as much as the lock. Once a blocking verb holds the display
the main loop drops repaints and the status-bar clock freezes, which looks
exactly like the ISSUE-30 wedge; a climbing count with no verb running is what
tells the two apart."
```

---

## Task 6 (WITHDRAWN — see the notice above): Bracket both dispatch paths

**Files:**
- Modify: `src/core/serialcmds.cpp:67`, `src/core/serialcmds.cpp:87`

**Interfaces:**
- Consumes: `tftLock()`, `tftUnlock()`, `TFT_LOCK_WAIT_FOREVER` from Task 5.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Add the include and the shared helper**

In `src/core/serialcmds.cpp`, add `#include "core/tft_lock.h"` with the existing includes. Then add this helper immediately above `handleSerialCommands()` (which begins at `:52`), next to `redrawUnlessNavigation()`:

```cpp
// Both dispatch paths take the display lock, not just one: a verb reached over
// BLE draws exactly as much as the same verb reached over HTTP, and it was the
// BLE path that produced both ISSUE-1 backtraces. One helper so the two cannot
// drift apart.
//
// WAIT_FOREVER, not a timeout: a dispatched verb legitimately owns the screen
// for its whole life. This cannot deadlock against the main loop, whose own
// bracket is short, self-contained and always releases.
static bool parseUnderDisplayLock(SerialCli &serialCli, const String &command) {
    tftLock(TFT_LOCK_WAIT_FOREVER);
    bool result = serialCli.parse(command);
    tftUnlock();
    return result;
}
```

- [ ] **Step 2: Use it on the HTTP `/cm` queue path**

Replace line 67:

```cpp
            bool result = serialCli.parse(String(packet.text));
```

with:

```cpp
            bool result = parseUnderDisplayLock(serialCli, String(packet.text));
```

- [ ] **Step 3: Use it on the BLE / serial line path**

Replace line 87:

```cpp
    bool result = serialCli.parse(cmd_str);
```

with:

```cpp
    bool result = parseUnderDisplayLock(serialCli, cmd_str);
```

- [ ] **Step 4: Verify no bare `parse()` remains**

```bash
grep -n "serialCli.parse(" src/core/serialcmds.cpp
```
Expected: exactly one hit, inside `parseUnderDisplayLock`.

- [ ] **Step 5: Build**

Run: `pio run -e smoochiee-board`
Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/core/serialcmds.cpp
git commit -m "fix(cli): hold the display lock across verb dispatch on both paths

A verb dispatched from the serial task draws to the same TFT over the same SPI
bus the main loop is drawing to, with nothing arbitrating -- the cross-task
mutex release behind ISSUE-1.

Bracketing every parse() rather than a whitelist of the six known
menu-dispatcher verbs is deliberate: there is no list to maintain and no gap
when a verb that draws is added later. Both call sites go through one helper
because missing either would leave a whole transport drawing unserialised, and
the BLE path is the one both recorded backtraces came in over."
```

---

## Task 7 (WITHDRAWN — see the notice above): Bracket the main-loop repaint and surface the counter

**Files:**
- Modify: `src/core/display.cpp:592-595`, `src/core/serial_commands/util_commands.cpp:105-130`

**Interfaces:**
- Consumes: `tftLock()`, `tftUnlock()`, `TFT_LOCK_DRAW_TIMEOUT_MS`, `tftLockTimeoutCount()` from Task 5.
- Produces: nothing.

- [ ] **Step 1: Bracket the 30 s status-bar repaint**

In `src/core/display.cpp`, add `#include "core/tft_lock.h"` with the existing includes. Then replace the block at `:592-595`:

```cpp
            if (millis() - _clock_bat_timer > 30000) {
                _clock_bat_timer = millis();
                drawStatusBar(); // update clock and battery status each 30s
            }
```

with:

```cpp
            if (millis() - _clock_bat_timer > 30000) {
                _clock_bat_timer = millis();
                // Skip the repaint rather than wait for it. A verb dispatched
                // from the serial task owns the display for its whole life, and
                // blocking here for minutes is what ISSUE-30 looked like from
                // the outside. A dropped frame is counted, so it is visible.
                if (tftLock(TFT_LOCK_DRAW_TIMEOUT_MS)) {
                    drawStatusBar(); // update clock and battery status each 30s
                    tftUnlock();
                }
            }
```

This is the right bracket point because ISSUE-30 already establishes it as the load-bearing one: it runs unconditionally every 30 s independent of input, which is why a frozen clock proves the task is blocked rather than merely idle.

- [ ] **Step 2: Report dropped frames through `free`**

In `src/core/serial_commands/util_commands.cpp`, add `#include "core/tft_lock.h"` with the existing includes. In `freeCallback()` (`:105`), extend the format string — append to the end of the literal, before the closing quote of the last fragment:

```
 | tftdrop=%u
```

and add the matching argument as the final parameter, after `(unsigned)ESP.getFreePsram()`:

```cpp
        (unsigned)tftLockTimeoutCount()
```

`free` is already where this repo goes for memory and contention truth, and the 320-byte `line` buffer has room.

- [ ] **Step 3: Build and check the tests still pass**

Run: `pio run -e smoochiee-board`
Expected: exit 0.

Run: `pio test -e native`
Expected: PASS, 28 cases.

- [ ] **Step 4: Commit**

```bash
git add src/core/display.cpp src/core/serial_commands/util_commands.cpp
git commit -m "fix(display): bound the main loop's wait for the display, and count drops

With verb dispatch now holding the display lock, the main loop must not block on
it: waiting portMAX_DELAY for a verb that owns the screen for minutes is exactly
the frozen-clock, dead-button, BLE-still-answering picture ISSUE-30 recorded, and
is why freeing memory did not unwedge it.

So the 30 s status-bar repaint waits 20 ms and skips. drawStatusBar is the right
place because ISSUE-30 established it as load-bearing -- it runs independent of
input, which is what makes a frozen clock evidence rather than idleness.

free now reports tftdrop. A frozen clock while a verb runs is now expected
behaviour, not a fault; a climbing tftdrop with no verb running is the wedge."
```

---

## Task 8 (WITHDRAWN — see the notice above): RC1-A hardware verification

**Files:** `docs/TEST_STATUS.md`, `docs/KNOWN_ISSUES.md`

**An idle test proves nothing.** ISSUE-1's own numbers are the bar: `deauth` survived 70+ s before dying on its first run, and `evilportal` took ~11 minutes under load. Every "survivor" in the seven-verb sweep was tested idle for 90 s and established nothing.

- [ ] **Step 1: Flash and record the baseline**

```bash
pio run -e smoochiee-board -t upload
sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9
```

Over BLE: `crashlog -clear`, then `free` — record the `tftdrop=` value (expected 0 at boot).

- [ ] **Step 2: `deauth` idle, past 130 s**

Capture the console (one `usbwatch2.py`). Dispatch `deauth` over BLE. Hold for **at least 180 s**.

Watch for: `assert failed: xTaskPriorityDisinherit`. Expect none.

Note the screen: the verb's menu should be displayed and the status-bar clock frozen. **That is now correct behaviour**, not a wedge.

- [ ] **Step 3: Release the verb and read the counters**

Press LEFT+RIGHT at the board — **twice**, because on a dimmed screen the first press is swallowed (`boards/smoochiee-board/interface.cpp:119-123` returns early when `wakeUpScreen()` reports it woke the display).

Then over BLE: `free`.

Expected: `tftdrop` **non-zero**. A zero count means the main loop never contended for the display and **the test proves nothing about the fix** — re-run with the device on the main menu so the 30 s repaint actually fires during the verb.

- [ ] **Step 4: `evilportal` under load, past 11 minutes**

Dispatch the **blocking** `evilportal` form. Associate a client and request portal pages continuously — this is the condition that produced the second backtrace; idle survived 100 s and meant nothing.

Hold for **at least 12 minutes**. Watch the console for the assert.

- [ ] **Step 5: `crashlog` after each run**

Even with the console captured, run `crashlog` over BLE after each of steps 2 and 4. It is the backstop for a panic nobody was watching for, and it is now the cheaper check.

- [ ] **Step 6: Record the outcome honestly**

Update `docs/KNOWN_ISSUES.md` ISSUE-1 and `docs/TEST_STATUS.md` with device date, ELF prefix, the durations actually reached, and the `tftdrop` counts.

Phrase it as **"no `xTaskPriorityDisinherit` assert observed in N s at tftdrop=M"** — never "fixed". The crash has never been reproduced on a build carrying any working lock, so there is no A/B baseline for the lock itself, and this can only ever be a negative result at a measured contention level.

If ISSUE-30 does not recur, say that it did not recur and that this is **suggestive, not conclusive** — it was observed once.

- [ ] **Step 7: Commit**

```bash
git add docs/KNOWN_ISSUES.md docs/TEST_STATUS.md
git commit -m "docs: record the display-lock runs against ISSUE-1's own thresholds"
```

---

## Task 9: Correct the ISSUE-1, ISSUE-30 and status docs

**Files:**
- Modify: `docs/KNOWN_ISSUES.md` (ISSUE-1 at `:23`, ISSUE-30 at `:2306`), `docs/TEST_STATUS.md`, `BRUCELINK.md`

Fixed entries move to §Resolved with the commit and the proving test rather than being deleted. VERIFIED vs SUSPECTED is tracked explicitly.

- [ ] **Step 1: Rewrite ISSUE-1's status and history**

Record, with evidence:
- `tftMutex` shipped in `2d9422ea` (2026-07-29 19:41) and `d2fc9d94` (23:24), and **no document mentioned it** until now.
- Both recorded backtraces are on ELFs `2841bf2b5` and `b02178b48`; TEST_STATUS records firmware `2d9422ea` as ELF `5186685c0fdf19c2`. Neither crash ELF is the fix build, so the evidence predates the fix — or the fix had a hole. Both resolved to the same action.
- The patch was unsound: 32 live `begin_tft_write` vs 57 live `end_tft_write`, 20 deliberate comment-outs, `drawRoundRect` among them and on this entry's own second backtrace.
- What replaced it (Tasks 4–7) and the Task 8 result, phrased as a negative result at a measured `tftdrop`.

- [ ] **Step 2: Add the `portMAX_DELAY` mechanism to ISSUE-30**

Add to the "Candidates, none discriminated" list at `:2341`: a `portMAX_DELAY` wait on `tftMutex`, noting that it accounts for every row of the evidence table — including the clean memory falsification, since a mutex wait is not a memory condition — and that the timing fits (mutex landed 2026-07-29 19:41, wedge observed 2026-07-30). Mark it **SUSPECTED**, and record that the revert removed the mechanism.

- [ ] **Step 3: Add the frozen-clock discrimination table**

To both `docs/TEST_STATUS.md` (Constraints) and `BRUCELINK.md` (Known gotchas), because BRUCELINK.md currently offers a frozen status-bar clock as *the* cheap liveness probe and that meaning has changed:

| Screen | Clock | Meaning |
|---|---|---|
| The verb's UI | frozen | Normal. The verb owns the display. |
| Main menu, no verb running | frozen | Wedge (ISSUE-30). |

Note that `free`'s `tftdrop` counter discriminates them.

- [ ] **Step 4: Update the "Broken — do not ship" rows**

In `docs/TEST_STATUS.md`, revise the `deauth` and `evilportal` (blocking) rows to state the lock's current status and the durations actually reached, rather than the flat "Crashes the device".

- [ ] **Step 5: Add the core dump to the testing docs**

In `BRUCELINK.md` §Testing and `docs/TEST_STATUS.md` §Test harness, state that a stored ELF core dump is readable with `crashlog` over BLE and that `usbwatch2.py` is **no longer the only place a backtrace appears**. Keep the `addr2line` snippet and the ELF-SHA256 warning — `crashlog` now supplies that prefix itself.

- [ ] **Step 6: Commit**

```bash
git add docs/KNOWN_ISSUES.md docs/TEST_STATUS.md BRUCELINK.md
git commit -m "docs: correct ISSUE-1 and ISSUE-30 for the display lock's real history

ISSUE-1 read OPEN with no fix while the fix had been in the tree since
2026-07-29 -- no document mentioned tftMutex at all. ISSUE-30's candidate list
never considered the portMAX_DELAY wait that explains its whole evidence table.
Both corrected, with the 32-vs-57 imbalance recorded as the reason the first fix
could not have worked."
```

---

## Task 10: The eight audit findings

**Files:**
- Modify: `docs/bruce-companion-api.md`, `docs/KNOWN_ISSUES.md`, `BRUCELINK.md`, `src/core/serial_commands/wifi_commands.cpp:74, 84, 101`

Unrelated to RC1/RC4, verified during the 2026-07-30 audit, and currently wrong. Two are code, six are docs.

- [ ] **Step 1: Add `attack_result` to the contract's frame union**

`docs/bruce-companion-api.md` §4.1: the `EventFrame` union omits `attack_result`, which ships from `pushAttackResult()` (`attack_commands.cpp:197-204`). Add it with its real fields — `verb`, `outcome`, `elapsed_ms`, `wifi_mode`, `free_heap` — and note `outcome` is `completed`, never `success`.

- [ ] **Step 2: Correct the `device_state` value set**

§4.1 says the values are `idle`, `portal`, `ble_spam` and "No other verb sets state"; §8 lists state frames as not built for anything but `evilportal`/`blespam`. Both are false: `runInteractiveAttack` calls `setDeviceState(verb)` (`attack_commands.cpp:208`) for `karma`, `deauth`, `blesniffer`, `ap_info`, `pwngrid`, and `reverseshell` sets its own at `:243`. Eight values, not three. Fix both sections and fix the stale citation `attack_commands.cpp:51,53,114,123`.

- [ ] **Step 3: Retire the "bare `return true`" warning**

The warning in §4.1 and in BRUCELINK.md §Known gotchas says every attack callback ends in a bare `return true`. `deauth` returns false on a target (`:230`) and `reverseshell` on AP failure (`:249`). Narrow the claim to `runInteractiveAttack` (`:212`), state why (the upstream entry points are `void`), and cross-reference ISSUE-7 as resolved.

- [ ] **Step 4: Fix ISSUE-28's and ISSUE-29's status headers**

ISSUE-28 (`:2097`) reads `OPEN (blocking verb only)` while its own body records the fix and `evil_portal.cpp:39` bails with `if (!apUp) return;`. Move it to §Resolved.
ISSUE-29 (`:2200`) reads `OPEN` while its body refutes its own premise — `taskInputHandler` clears the globals every ≤75 ms. Mark it **WITHDRAWN** with the reason, not resolved.

- [ ] **Step 5: Fix the `arp` and `listen` error paths**

`src/core/serial_commands/wifi_commands.cpp:84` (`scanHostsCallback`) and `:101` (`listenTCPCallback`) print their only error text to `Serial`, which reaches nothing (ISSUE-22) — the same defect as the just-fixed ISSUE-42. Both verbs are live on this board: `boards/smoochiee-board/smoochiee-board.ini` extends `[env]`, not `[env_light]`, so `LITE_VERSION` is undefined and they are registered at `:136` and `:139`.

Change both to `serialDevice->println(...)`. Do **not** touch `navCallback`'s `Serial` use — that one is deliberate and documented at `util_commands.cpp:238-242`.

- [ ] **Step 6: Fix the `webui` precedence bug**

`wifi_commands.cpp:74`:

```cpp
    serialDevice->println(String("Starting Web UI ") + !noAp ? "AP" : "STA");
```

`+` binds tighter than `?:`, so the string concatenation is the ternary's *condition* and the app receives a bare `"AP"` or `"STA"`. Present in upstream at `59e83bfb:61`. Fix:

```cpp
    serialDevice->println(String("Starting Web UI ") + (!noAp ? "AP" : "STA"));
```

- [ ] **Step 7: Build, test, commit**

```bash
pio run -e smoochiee-board   # expect exit 0
pio test -e native           # expect PASS, 28 cases
```

```bash
git add src/core/serial_commands/wifi_commands.cpp
git commit -m "fix(wifi): send arp and listen errors where the app can read them

Both verbs are registered on this board -- smoochiee-board.ini extends [env],
not [env_light], so LITE_VERSION is undefined -- and each printed its only error
text to Serial, which reaches nothing here. Over BLE they returned a bare
Result: FALSE with no reason, the same defect as ISSUE-42.

Also fix the webui reply: + binds tighter than ?:, so the concatenation was the
ternary's condition and the client received a bare \"AP\" instead of
\"Starting Web UI AP\". Upstream bug, present at 59e83bfb."
```

```bash
git add docs/bruce-companion-api.md docs/KNOWN_ISSUES.md BRUCELINK.md
git commit -m "docs: correct the contract's event surface and two stale statuses

The contract omitted attack_result entirely while the firmware emits it, and
claimed device_state has three values when runInteractiveAttack sets it for six
more verbs -- an app switching on it would meet values the contract never named.
The bare-return-true warning was also stale. ISSUE-28's header said OPEN while
its body and evil_portal.cpp:39 record the fix; ISSUE-29 is withdrawn, since its
own body refutes its premise."
```

- [ ] **Step 8: Bump the contract version**

`docs/bruce-companion-api.md:9` reads "Contract version: 2.3 — audited against `0b2073fa`", which is 45 commits behind. Bump to 2.4, set the audit point to the current HEAD, and summarise what changed: `attack_result`, the eight `device_state` values, the ISSUE-7 correction, and `crashlog`. The file's own instruction is "Bump this line whenever the contract changes."

```bash
git add docs/bruce-companion-api.md
git commit -m "docs: bump the API contract to 2.4 and re-anchor its audit point"
```

---

## Self-review notes

Checked against the spec:

- **Spec coverage.** RC4 → Tasks 1–3. RC1-A revert → Task 4. Lock module → Task 5. Both parse brackets → Task 6. Repaint bracket and counter → Task 7. Verification thresholds → Tasks 3 and 8. Doc corrections → Tasks 9 and 10. The spec's *Honest limits* (a wedge writes no core dump; `CAPTURE_DRAM` off; 64 KB overwritten by a later crash) are reflected in Task 3 step 1 and Task 8 step 5-6 phrasing. **Out of scope and deliberately absent:** RC1-B, RC2, RC3, RC5.
- **Type consistency.** `CrashSummaryView` field names are identical in Task 1's test, Task 1's header and Task 2's consumer. `tftLock`/`tftUnlock`/`tftLockInit`/`tftLockTimeoutCount` and both constants are spelled identically in Tasks 5, 6 and 7. `formatCrashSummary` and `resetReasonName` match between Tasks 1 and 2.
- **Known gap, accepted:** `tftLock()` returning `true` before `tftLockInit()` means an early caller is unprotected. That window is single-tasked by construction — `tftLockInit()` runs at `main.cpp:474` and the first drawing task is created at `:533` — but it is a silent contract rather than an enforced one.
