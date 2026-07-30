# Crash evidence and the display lock — design

**Date:** 2026-07-30 · **Baseline:** HEAD `3629afd7`, tree clean, build exit 0,
`pio test -e native` 21/21.

Two changes, in this order:

1. **RC4 — a crash-evidence path.** Read the ELF core dumps this board has been
   writing to flash all along, and report them over BLE.
2. **RC1-A — a correct display lock.** Replace the unsound `tftMutex` patch with an
   explicitly paired lock at the two sites that actually collide.

RC4 lands first because it makes RC1-A's verification cheap: today, proving anything
about a crash requires an operator with `usbwatch2.py` already running at the moment
it happens.

---

## Why

### RC4 — the evidence was already being recorded

`custom_16Mb.csv` declares a 64 KB `coredump` partition at `0xFF0000`. The framework
sdkconfig already sets `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`,
`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y` and `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`, and
`libespcoredump.a` is linked.

`grep -rn "esp_core_dump\|esp_reset_reason" src/ include/` returns **nothing**.

Every panic on this device has written a full ELF core dump to flash that nothing has
ever read. Two open entries are stuck for exactly the reason this closes:

- **ISSUE-11** — an unexplained reboot, observed once, "no console capture".
- **ISSUE-30** — a main-loop wedge whose entry states "**Why no backtrace exists:**
  nothing panics and no watchdog fires". (A wedge with no panic writes no core dump
  either — see *Honest limits* below.)

On Xtensa the backtrace is generated **on-device** — `esp_core_dump_bt_info_t` carries
`bt[16]`, `depth` and `corrupted`
(`espcoredump/include/port/xtensa/esp_core_dump_summary_port.h`). And
`esp_core_dump_summary_t` carries `app_elf_sha256[APP_ELF_SHA256_SZ]` where
`APP_ELF_SHA256_SZ = CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1` and
`CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9` — a NUL-terminated 9-character ASCII prefix, the
**same form the panic handler prints** and the same form the register already records
(`2841bf2b5`, `b02178b48`). It needs no conversion to be compared against a local
build.

That matters because the repo's own rule is "Check the panic's `ELF file SHA256`
against the local ELF before trusting a decode." The core dump delivers the identifier
*with* the backtrace instead of relying on the two being reconciled by hand.

### RC1-A — the ISSUE-1 fix shipped, and it is unsound

`tftMutex` is a global recursive mutex declared at `lib/TFT_eSPI/TFT_eSPI.cpp:72` and
created at `:646`. It is taken and given in all six transaction entry points:
`begin_tft_write`/`end_tft_write` (`:95`/`:126`), `begin_nin_write`/`end_nin_write`
(`:109`/`:143`) and `begin_tft_read`/`end_tft_read` (`:164`/`:187`). It arrived in fork
commits **`2d9422ea`** (2026-07-29 19:41) and **`d2fc9d94`** (2026-07-29 23:24).

**No document mentions it.** `grep -n tftMutex docs/*.md BRUCELINK.md` is empty.
ISSUE-1 still reads `Status: OPEN · Severity: critical`; TEST_STATUS.md still lists
`deauth` and `evilportal` under "Broken — do not ship"; BRUCELINK.md still carries the
crash as a live gotcha.

The scheme is a per-call take/give plus a second take tied to the SPI transaction's
lifetime, so that a batching caller (`inTransaction = true`) cannot let a second task
end a transaction it did not begin. The reasoning is right. The premise is not:
**begin and end are not paired in TFT_eSPI, by design.**

- **32** live `begin_tft_write()` calls against **57** live `end_tft_write()` calls.
- **20** sites comment the begin out deliberately —
  `//begin_tft_write(); // Sprite class can use this function, avoiding begin_tft_write()`
  — at `:2330, 2404, 2472, 2548, 2600, 2647, 2668, 2694, 2716, 2758, 2815, 2839, 2862,
  2886, 3280, 3318, 3411, 3551, 3938, 5355`.
- `drawArc:4133` sets `inTransaction = true` with no begin at all.

So the trailing unconditional `xSemaphoreGiveRecursive(tftMutex)` in `end_tft_write`
executes roughly 25 times more often than its matching take. FreeRTOS
`xQueueGiveMutexRecursive` returns `pdFAIL` when the caller is not the holder — which
is why the display works today — but **decrements when it is**. Any path where an
unmatched give lands while an outer transaction legitimately holds a count of 1
releases the display mid-transaction and restores precisely the cross-task race
ISSUE-1 describes.

`drawRoundRect` (`:2668`) is one of the 20, and it is on ISSUE-1's *second* backtrace:

```
drawStatusBar → drawBatteryStatus → drawRoundRect
              → TFT_eSPI::end_tft_write → SPIClass::endTransaction
              → spiEndTransaction → assert failed: xTaskPriorityDisinherit
```

The invariant currently holds by luck — unmatched gives happening to fail — not by
design.

**Why not fix it in place.** `locked` is TFT_eSPI's single-threaded *reentrancy* flag.
It cannot express "another task owns the bus." Every in-library variant tried during
design either reintroduced an unbalanced give, or let a second task pass the `if
(locked)` test and draw while the first task held the lock. The patch is not a small
slip in the right place; the lock is in the wrong place.

### ISSUE-30 is plausibly iatrogenic

Both takes use `portMAX_DELAY`, so a leaked hold blocks every other task on the
display **forever**. ISSUE-30 was observed **2026-07-30** — after the mutex landed on
2026-07-29 19:41 — and `tftMutex` is not among the candidates its entry lists.

A `portMAX_DELAY` wait accounts for every row of that entry's evidence table: static
screen, frozen status-bar clock, dead buttons, BLE answering in 60–122 ms, no panic,
no watchdog, and — the part that defeated the memory hypothesis — **freeing memory did
not unwedge it**. A mutex wait is not a memory condition, which is exactly why that
falsification came back clean without identifying a cause.

This is a **hypothesis**, not a verified cause. ISSUE-30 was observed once and has not
recurred. It is recorded here because it changes the fix (a bounded wait, below) and
because the entry's candidate list should include it.

---

## Scope

**In scope:** RC4 in full; RC1-A in full; the doc corrections listed at the end.

**Out of scope, deliberately:**

- **RC1-B — moving menu-verb execution onto the display-owner task.** The structural
  end-state: with one task drawing, the race is gone rather than arbitrated, and the
  freed serial task also buys ISSUE-19b (a BLE rescue path), the `/cm` HTTP 400s and
  much of ISSUE-6. It needs an injection point inside `mainMenu.begin()` and touches
  upstream menu flow, so it gets its own spec after RC1-A is proven on hardware.
  Bundling an upstream control-flow change into a crash fix would make the crash fix
  unverifiable.
- **RC2** (latched consume-once press events, for ISSUE-19a/29), **RC3** (one
  diagnostic sink plus a generalized admission gate, for ISSUE-12/16/25 and the
  `arp`/`listen` bug), **RC5** (runtime probes, for ISSUE-3/4/36/37). Analysed, not
  designed here.

---

## RC4 — architecture

### Components

**`src/core/crash_report.h`** — new, header-only, **pure logic, no Arduino or IDF
dependency**. Follows the `modules/wifi/portal_cap.h` precedent so it is testable
under `[env:native]`, which builds with `test_build_src = no` and `-I src`.

```cpp
struct CrashSummaryView {
    const char *taskName;      // may be empty
    uint32_t    excPc;
    const uint32_t *bt;        // may be null when depth == 0
    uint32_t    depth;         // clamped to btCapacity by the formatter
    uint32_t    btCapacity;    // 16 on Xtensa
    bool        corrupted;
    const char *elfSha256;     // NUL-terminated 9-char ASCII, may be empty
};

std::string formatCrashSummary(const CrashSummaryView &v);
```

`std::string` rather than Arduino `String` so the native environment can build it;
`std::string` is already used on-device (`BLESerialService.cpp:26`).

Output shape, one line per field so it survives BLE chunking and stays greppable:

```
crash: task=main pc=0x420a1c34 elf=2841bf2b5 depth=7 corrupted=no
crash: bt=0x420a1c34 0x420a0f88 0x4037d1a0 0x4037c904 ...
```

**`src/core/serial_commands/crash_commands.{cpp,h}`** — new. `createCrashCommands(SimpleCLI*)`
called from `cli.cpp`, matching how every other area registers. Whole file inside:

```cpp
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
```

with an `#else` that registers a verb reporting core dumps are not compiled in — so
the verb never silently vanishes on another board profile. This is the ISSUE-4 lesson
applied: a missing capability must say so rather than be absent.

| Verb | Behaviour |
|---|---|
| `crashlog` | `esp_core_dump_image_get()` first. `ESP_ERR_NOT_FOUND` → `crash: none stored`. Present → `esp_core_dump_get_summary()` → `formatCrashSummary()` → `*serialDevice`. |
| `crashlog -clear` | `esp_core_dump_image_erase()`; reports the `esp_err_t` either way. |

**Boot reporting**, at the end of `setup()` alongside the existing
`bleApiAutoStart` re-arm (`main.cpp:588`): push `esp_reset_reason()` as a `log` frame,
and if `esp_core_dump_image_get()` reports a dump, one further line naming the
faulting task. Costs one flash read on a clean boot.

`esp_reset_reason()` is worth having on its own: it distinguishes the
`rst:0x1 (POWERON)` / `rst:0xc (RTC_SW_CPU_RST)` pair that ISSUE-30 records as
"calibration worth keeping", and makes it available to the app rather than only to
someone reading a console.

### Output goes to `*serialDevice`, never `Serial`

The whole point is to reach the app when no console is attached. `Serial` reaches
nothing on this board (ISSUE-22), which is what made ISSUE-42 a defect and what
currently makes `radio_mem.h`'s decisions unobservable.

### Honest limits

- **A wedge writes no core dump.** ISSUE-30 is a hang with no panic and no watchdog,
  so nothing triggers a dump. RC4 does not close ISSUE-30. What it does close is
  ISSUE-11 (an actual reboot) and it makes every *future* panic self-reporting —
  including any RC1-A regression. ISSUE-30's own next step remains the `log_e`
  `Selected:`/`Returned from:` pair already armed in `411d7e151dbc2356`, now joined by
  the `tftLockTimeouts` counter below.
- **`CONFIG_ESP_COREDUMP_CAPTURE_DRAM` is not set**, so the dump holds task stacks and
  registers, not the full data segment. Sufficient for a backtrace; not for inspecting
  heap contents.
- **64 KB, and `FLASH_NO_OVERWRITE` is not set**, so a later crash overwrites an
  earlier one. `crashlog` should be read before deliberately crashing the device
  again. This is a workflow note, not a defect.

---

## RC1-A — architecture

### Revert first

Remove the `xSemaphoreTakeRecursive`/`xSemaphoreGiveRecursive` calls added by
`2d9422ea` and `d2fc9d94` from all six entry points in `lib/TFT_eSPI/TFT_eSPI.cpp`,
returning them to upstream behaviour, and delete the now-false explanatory comment at
`:80-94`. Keep `tftMutex` itself out of the library entirely.

Reverting an unsound guard is not a regression: it removes ~25 unmatched gives and the
`portMAX_DELAY` wedge mechanism in one step, and leaves ISSUE-1 exactly where the
register already believes it is.

### Then lock where the collision is

**`src/core/tft_lock.{h,cpp}`** — new, additive, per the fork's "prefer new files"
convention.

```cpp
bool tftLock(uint32_t timeoutMs);   // false = not acquired, caller must not draw
void tftUnlock();
uint32_t tftLockTimeoutCount();
```

A plain recursive mutex created during display init, with **no coupling to `locked` or
`inTransaction`**. Those stay what upstream intended: per-owner reentrancy state,
touched only by whichever task currently holds the lock.

Two brackets, both in code this fork owns:

| Site | Wait | On failure |
|---|---|---|
| `serialcmds.cpp:67` — the HTTP `/cm` queue path's `serialCli.parse()` | `portMAX_DELAY` | n/a. A dispatched verb legitimately owns the screen for its whole life. |
| `serialcmds.cpp:87` — the BLE/serial line path's `serialCli.parse()` | `portMAX_DELAY` | as above |
| `display.cpp:592-595` — the main loop's 30 s `drawStatusBar()` repaint inside `loopOptions()` | **bounded, 20 ms** | Skip the repaint, bump `tftLockTimeouts`, continue. |

**There are two `parse()` call sites, and both must be bracketed.** Missing either
leaves a whole transport able to draw unserialised — and `:87` is the BLE path, which
is the one ISSUE-1's backtraces were dispatched over (`_serialCmdsTaskLoop →
handleSerialCommands → SimpleCLI::parse`). Extracting a single
`parseUnderDisplayLock(SerialCli&, const String&)` helper used by both is preferable to
two hand-written brackets that can drift apart.

`portMAX_DELAY` on the serial side cannot deadlock: the main loop's bracket is short
and self-contained, so it always releases. The reverse is guarded by the timeout.

Bracketing **every** `parse()` rather than a whitelist of the six known
menu-dispatcher verbs is deliberate — there is no list to maintain and no gap when a
verb that draws is added later. The cost is that the main loop drops repaints during
any command, which the bounded wait makes harmless.

`drawStatusBar()` is the right bracket point on the main-loop side because ISSUE-30
already establishes it as the load-bearing one: it runs unconditionally every 30 s
independent of input, which is why a frozen clock proves the task is blocked rather
than merely idle.

### A consequence that must be documented, not discovered

While a blocking verb holds the lock — `evilportal` for 11 minutes, `deauth`
indefinitely — the main loop drops **every** repaint. The status-bar clock therefore
freezes.

That is correct: the verb owns the screen and the main loop must not paint over its
UI. But BRUCELINK.md currently offers a frozen status-bar clock as *the* cheap
liveness probe for ISSUE-30, so the probe's meaning changes and the docs must say so:

| Screen | Clock | Meaning |
|---|---|---|
| The verb's UI | frozen | Normal. The verb owns the display. |
| Main menu, no verb running | frozen | Wedge (ISSUE-30). |

`tftLockTimeoutCount()` discriminates them: a counter climbing while no verb runs
means the main loop is being denied the display by something that has not released it.
Exposed through the **`free`** verb (`util_commands.cpp:455`), which is already the
place this repo goes for memory and contention truth.

### Residual sharp edge, stated plainly

`inTransaction` is still assigned by callers outside any lock — `drawArc:4133` and the
19 other sites set it before any take. With both drawing paths bracketed, only one
task is inside TFT_eSPI at a time, so no concurrent assignment occurs *through the two
known sites*. A third drawing site added later would not be covered.

This is why RC1-A is a correct fix for a known collision and **not** a claim that
TFT_eSPI is thread-safe. RC1-B remains the structural answer.

---

## Testing

### Host-side, `pio test -e native`

New `test/test_crash_report/` against `crash_report.h`, following
`test/test_portal_cap/`:

- `depth == 0` with a null `bt` renders no `bt=` line and does not dereference.
- `depth > btCapacity` clamps to `btCapacity` rather than over-reading.
- `corrupted == true` renders `corrupted=yes`.
- An empty `taskName` and an empty `elfSha256` render without producing a malformed
  line.
- A known `bt[]` renders lower-case `0x%08x`, space-separated, in order.

The lock is FreeRTOS; it is not host-testable and gets no native test.

### On hardware

RC4, first flash cycle:

1. `crashlog` on a clean device → `crash: none stored`.
2. Provoke a panic with `deauth` on the **current** build — the crash the register
   already documents — with the console captured as a control.
3. After reboot, `crashlog` **over BLE, console unplugged**. Success is a task name,
   a PC, a backtrace, and an `elf=` value matching the local ELF's 9-character prefix.
4. Cross-check the decoded PCs against the console capture from step 2. The two must
   agree; that is what makes RC4 trustworthy for every later use.
5. `crashlog -clear`, then `crashlog` → `crash: none stored`.

RC1-A, second flash cycle. **An idle test proves nothing here** — ISSUE-1's own
thresholds are the bar:

6. `deauth`, idle, past **130 s**. It died at 70–130 s on its first run, so anything
   shorter is not evidence.
7. `evilportal` (blocking form) **under load** — a client associated and pages being
   requested — past **11 min**, the point the second backtrace was captured.
8. `free` after each: report `tftLockTimeouts`. A non-zero count is the good outcome —
   it proves contention occurred and was handled. Zero means the test did not
   exercise the collision and proves nothing about the fix.
9. `crashlog` after each, as a backstop for a panic nobody was watching for.

### What is expected to be uncertain

- **Whether RC1-A actually stops the ISSUE-1 crash.** The crash has never been
  reproduced on a build carrying any lock, so there is no A/B baseline for the lock
  itself. Steps 6–8 can only show "no crash observed in N s at a measured contention
  count" — never "fixed". Report it that way.
- **Whether ISSUE-30 was the `portMAX_DELAY` wedge.** It was observed once and has not
  recurred. If it never recurs after the revert, that is suggestive and nothing more,
  and the entry should say so rather than being closed.
- **The 20 ms timeout.** Chosen so a skipped repaint is cheaper than a stalled main
  loop. `tftLockTimeouts` makes it measurable; expect to tune it once.

---

## Docs to correct

Correcting these is part of the work, not follow-up. Ordered by how badly the current
text misleads:

1. **ISSUE-1** — record that `tftMutex` shipped in `2d9422ea`/`d2fc9d94`, that both
   recorded backtraces are on ELFs (`2841bf2b5`, `b02178b48`) that are not the fix
   build (`5186685c0fdf19c2`), that the patch is unsound with the 32-vs-57 count as
   evidence, and what replaced it.
2. **ISSUE-30** — add the `portMAX_DELAY` mechanism to the candidate list, and the
   revised meaning of the frozen-clock probe.
3. **TEST_STATUS.md** — the `deauth` / `evilportal` "Broken — do not ship" rows, and
   the new frozen-clock table.
4. **BRUCELINK.md** — the ISSUE-1 gotcha, the liveness-probe gotcha, and a line under
   **Testing** stating that a core dump is available and `usbwatch2.py` is no longer
   the only place a backtrace appears.
5. **The eight audit findings from 2026-07-30**, unrelated to RC1/RC4 but verified and
   currently wrong in the docs: the API contract's missing `attack_result` frame; the
   8-value `device_state` set (`attack_commands.cpp:208,243`) against §4.1's three and
   §8's claim that state frames are not built; the stale "every attack callback ends in
   a bare `return true`" warning in §4.1 and BRUCELINK.md; ISSUE-28's header saying
   OPEN when its body and `evil_portal.cpp:39` record the fix; ISSUE-29 needing
   WITHDRAWN rather than OPEN; the `arp`/`listen` verbs' only error text going to
   `Serial` (`wifi_commands.cpp:84,101`); and the upstream `webui` precedence bug
   (`wifi_commands.cpp:74`) that sends the app a bare `"AP"`.

---

## Success criteria

- `pio run -e smoochiee-board` exit 0; `pio test -e native` green including
  `test_crash_report`.
- `crashlog` returns an ELF-matched backtrace **over BLE with no console attached**,
  agreeing with a console capture of the same crash.
- `lib/TFT_eSPI/TFT_eSPI.cpp` contains no semaphore calls; `grep -c
  xSemaphore.*tftMutex lib/TFT_eSPI/TFT_eSPI.cpp` is 0.
- `deauth` idle past 130 s and `evilportal` under load past 11 min with no
  `xTaskPriorityDisinherit` assert, at a **non-zero** `tftLockTimeouts`.
- The four docs and the eight audit findings corrected, with every claim carrying a
  `file:line` or a device-and-date.
