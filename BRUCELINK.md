# BRUCELINK.md

Agent memory and living architecture doc for this repository. Read it before making
changes. If anything here conflicts with a doc under `docs/`, the more recently
verified one wins — check the dates.

## Project

**brucelink-firmware**: a personal fork of [Bruce](https://github.com/pr3y/Bruce)
ESP32 firmware, modified so a **mobile companion app** can drive the device remotely.
Upstream Bruce is driven from its own screen and buttons; this fork exposes the same
capabilities over a command bus reachable via BLE GATT and WiFi HTTP.

- **Fork point:** upstream `59e83bfb`. Fork work starts at `373fb5d8`.
- **Target board:** `smoochiee-board` env — ESP32‑S3‑N16R8, 16 MB flash, 8 MB OPI PSRAM.
- **Companion app:** `/home/kevsmir07/Projects/brucelink` (React Native/Expo;
  renamed from `maritest`). It
  vendors a copy of the API contract in `vendor/`; if you change the interface,
  that copy goes stale.

### Build and test

```sh
pio run -e smoochiee-board                 # build
pio run -e smoochiee-board -t upload       # flash
pio test -e native                         # host-side unit tests (Unity)
```

`[env:native]` builds pure-logic files only (`test_build_src = no`). Anything that
touches Arduino, FreeRTOS or hardware cannot be tested there — see **Testing**.

## The one thing to understand first

Every WiFi attack in Bruce tears down the WiFi stack. So the obvious remote-control
path — the existing Web UI over HTTP — destroys itself the moment you use it for the
thing you wanted it for. **BLE is therefore the primary control transport**, with WiFi
as bulk transfer only.

But BLE and WiFi fight over the same scarce resource: internal DMA-capable RAM. PSRAM
**cannot** back the BT controller. `radioHasMemForBle()` (`core/radio_mem.h`) admits a
BLE attack only when the largest **contiguous DMA block** clears 15 KB, and when the
check fails its fallback tears down WiFi to make room. So with the BLE API up it is
plausibly **the memory guard, not the attack**, that kills the AP — but note that when
this was actually observed (`blesniffer` destroying a live AP, ISSUE-19) *neither* of
the guard's log lines reached the console, so the attribution is **SUSPECTED**, not
verified. The competing path is ruled out: `FORCE_RADIO_TEARDOWN_ON_SWITCH` is `false`
here (`ble_common.h:32-40`, disabled by an `#if 0`).

Consequences that shape most of the code:

- Free heap does not predict this. Use the `free` verb, which reports the largest
  contiguous DMA block explicitly.
- **The two transports coexist, but with almost no margin.** Measured 2026-07-29:
  from a *fresh boot* `webui` starts fine and `systeminfo` answers over BLE with the
  AP up (`free_heap:14140`), and a laptop associates and gets a DHCP lease. From a
  boot where anything already took ~18 KB, the same command starts nothing: AsyncTCP
  fails to allocate with 1,235 bytes free, the AP beacons but cannot accept a station,
  and BLE replies truncate to 1 byte. See `docs/KNOWN_ISSUES.md` §ISSUE-12.
  **This entry first claimed the transports were mutually exclusive; that was wrong,
  generalised from one run launched off a dirty baseline.**
  **Two further corrections, 2026-07-31.** *"`webui -bg` reports success either way"* is
  no longer true — RC3 gates the start and reports a real outcome on four channels, and
  `[CLI] Result: FALSE` from `webui` now means it is not serving. And *"a single `js` run
  is enough"* is no longer true either: ISSUE-17 is resolved — the ~17.8 KB is the
  interpreter task's stack and the FreeRTOS idle task reclaims it within one BLE round
  trip, so `js` then `webui` as separate commands measures **no** heap difference. What
  RC3 does **not** fix is the marginal case where the listener starts and a *request's*
  allocation fails; ISSUE-12 stays OPEN for that.
- Never let both radios be loaded at once. `blespam` suspends the BLE API *before*
  touching NimBLE, which frees ~62 KB and takes the DMA block from ~1.3 KB to ~32 KB.
- Ordering is everything. Tearing BLE down cleanly *before* an attack means the
  rebuild is a fresh `setup()`; rebuilding *after* an attack mangled the stack was
  tried and reverted (`eb05177b` → `e2631370`, crashed).

## Quality bar

This repo's standard is **evidence, not assertion**. The user's standing instruction
is *"FACTS OVER CLAIMS — test first, audit first before making any claims."*

- **Never state a behaviour you have not verified.** Cite `file:line` for a code
  claim, or give device + date for a measurement. Mark anything else **UNVERIFIED**.
- **A clean test window is not proof of safety.** `deauth` survived 70+ s before
  crashing on its first run. Say "no crash observed in N s", never "safe".
- **Distinguish VERIFIED from SUSPECTED** when generalising from one tested case to
  an untested one that looks similar. Test each; do not infer.
- **Quote the conditions with a measurement.** "Largest DMA block 1,332 bytes" is
  meaningless without "fully loaded, with a station associated". The same board reads
  6,900 with no station.
- **Report failures faithfully.** If something is blocked, skipped, or untested, say
  so explicitly rather than quietly narrowing scope.

Correct the docs when you find them wrong. That is expected work, not scope creep.

## Conventions

- **Comments explain *why*, never *what*.** This codebase's comments carry the reason
  a non-obvious guard exists — the DMA ordering, the mutex, the `SO_REUSEADDR`. Match
  that density; do not add narration.
- **No AI attribution in commits.** Never `Co-Authored-By:` for an assistant, never a
  "Generated with…" line. Configured off globally; do not reintroduce.
- **Commit style**: `type(scope): imperative summary`, lowercase. Types in use:
  `feat`, `fix`, `docs`, `tools`, `test`, `refactor`. The body explains the *symptom*
  that motivated the change, not just the change.
- **Formatting**: `.clang-format` (LLVM base, 4-space indent, 110 col). Run it.
- **Upstream files**: this is a fork that intends to stay mergeable. Prefer additive
  changes in new files (`attack_commands.cpp`, `ws_events.cpp`, `system_info.cpp`)
  over edits scattered through upstream code.

## Architecture

### The command bus

One CLI (`SerialCli::parse`, SimpleCLI) is the single command surface. **Every verb
works identically over either transport** — there is no BLE-only or HTTP-only verb.

Verbs are registered per area in `src/core/serial_commands/*.cpp`, each exposing a
`createXCommands(SimpleCLI*)` called from `cli.cpp`. `attack_commands.cpp` is this
fork's file; the rest are upstream.

`serialDevice` is a global `SerialDevice*` that **rebinds at runtime**: `&USBserial`
normally, `&serial_service` (the BLE GATT service) while the BLE API is armed
(`ble_api.cpp:63`). This is the single most common source of confusion:

- Output written to `Serial` instead of `*serialDevice` **never reaches the app**.
  `settings` used to do this — `settings_commands.cpp` serialised to `Serial` and the
  app saw an empty reply; **fixed in `b1c825c8` (ISSUE-2 resolved)**, it now goes to
  `*serialDevice`. The lesson stands: anything the app must read goes through
  `*serialDevice`, never bare `Serial`.
- While BLE is armed, the USB CDC port accepts **no** CLI input. It still carries
  `ESP_LOG`/panic output, which is why the USB console is the only place a crash
  backtrace appears.

### Dispatch and the blocking problem

`handleSerialCommands()` (`serialcmds.cpp`) runs on its own FreeRTOS task
(`_serialCmdsTaskLoop`) and calls `serialCli.parse()` **inline**. Two entry paths:

1. HTTP `POST /cm` → a depth-2 queue → drained by the serial task.
2. BLE → bytes land in a 512-byte ring via `pushRx` on the **NimBLE host task**, and
   the serial task drains complete lines with `hasLine('\n')`.

Because `parse()` is inline, a verb that opens a TFT menu holds the serial task for
its entire life. While blocked, **no BLE command is parsed at all** — the bytes
arrive and sit unread. HTTP `/cm cmnd=nav esc` is the exception: it is special-cased
in the AsyncWebServer task *before* queueing and writes the button globals directly
(`webInterface.cpp:532`).

`COMMAND: <verb>` is pushed to the event stream **before** `parse()` and
`[CLI] Result:` **after** — so a dispatch ACK reaches the app even for a verb that
then blocks for minutes. But see the warning in **Known gotchas** about what
`Result:` actually means.

### BLE transport (`src/modules/ble_api/`)

Advertises as `Bruc`. Written out in full because the service and the two
characteristics share only their *first* field — abbreviating them to `d555ed97-…`
once led to the characteristics being written with the service's suffix, giving a
bench script two UUIDs that do not exist on the device:

| | UUID | Properties |
|---|---|---|
| Service | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` | — |
| CLI characteristic | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` | READ\|NOTIFY\|**WRITE** |
| Event characteristic | `d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9` | READ\|NOTIFY |

Source of truth: `services/BLESerialService.cpp:70,80`. Note the CLI characteristic
does **not** advertise `WRITE_NR`, so a client must write **with** response —
`bleak`'s `response=False` is rejected against this GATT table.

- **Commands** are newline-terminated. **Responses** end with a `0x04` EOT byte —
  *not* the `"# "` prompt, because any CLI output line beginning with `# ` (a dumped
  markdown file, a commented config) would truncate the reply and desynchronise every
  command after it.
- Notifies are chunked to `negotiated_MTU - 3`. The app **must** request MTU 247;
  Android's default 23 yields 20-byte chunks.
- `ByteRing` (`services/ByteRing.h`) is the RX FIFO, guarded by a FreeRTOS mutex
  because `pushRx` (NimBLE task) and reads (serial task) genuinely race. It is the
  one piece with host-side unit tests.
- Events go on their **own** characteristic so async JSON never interleaves with CLI
  stdout. Events are dropped, not queued, when nobody is subscribed.
- Teardown must use `NimBLEDevice::deinit(true)`. The default leaves services
  allocated, producing duplicate characteristics with the same UUID on each
  arm/disarm cycle. The same default is why `blespam` used to come back nameless:
  `deinit(false)` also keeps the advertising payload, and `bleApi.setup()` then
  appended the name and UUID on top of it until the 31-byte advert overflowed
  (`c9c43c03`). Anything handing the radio back must leave the advert empty *and*
  restore the BT MAC it rotated.

### Event stream (`src/core/wifi/ws_events.cpp`)

`pushWsEvent()` builds one frame and fans it out to both `/ws` and the BLE event
characteristic. `wsEventId` increments **unconditionally**, even with nobody
listening, so the ID space is gap-free across a transport switch and an app tracking
`lastEventId` can detect what it missed. There is **no** server-side replay; that was
judged not worth the RAM.

Frames actually emitted: `state`, `log`, `ble_progress`, `ble_result` and
**`attack_result`**. Nothing else, whatever older design notes claim.

`attack_result` was missing from this list until 2026-07-30, when it was captured live on
the event characteristic while `ap_info` was released by the HTTP `nav` rescue:

```json
{"id":12,"type":"attack_result","verb":"ap_info","outcome":"completed",
 "elapsed_ms":109994,"wifi_mode":2,"free_heap":7699}
```

It is emitted by `pushAttackResult()` — e.g. `attack_commands.cpp:253` on a
`reverseshell` AP failure. It carries the only *real* outcome signal the attack verbs
have, which matters because `[CLI] Result: TRUE` does not mean success (see
**Known gotchas**).

### Boot

`main.cpp` re-arms the BLE API at the **end** of `setup()`, after WiFi init, if
`bruceConfig.bleApiAutoStart` is set (`main.cpp:588`). This is opt-in and runs last —
it is *not* the reverted `eb05177b`, which ran unconditionally *before* WiFi to win
the DMA race and crashed.

### Diagnostics

`RAM_LOG()` stage markers are compiled in (`-D ENABLE_RAM_LOGGING`) and print a full
heap profile at each subsystem transition. The periodic **sampler** is separately
opt-in (`-D ENABLE_RAM_SAMPLER=1`) because its 4 KB task stack comes out of internal
DRAM — larger than the margin it would be measuring. A diagnostic that changes the
result is worse than no diagnostic.

## Hardware reality on the reference unit

The `smoochiee-board` profile describes a **fully populated Smoochiee V2**. The actual
test unit is a bare ESP32‑S3‑N16R8 devkit with an IPS LCD and five buttons — nothing
else. Therefore:

- **`/systeminfo` `capabilities` are compile-time `#define` checks, not runtime
  probes.** They report `has_cc1101`, `has_nrf24`, `has_gps`, `has_ir`, `has_buzz`,
  `has_rgb_led`, `has_mic` all `true` on a board with none of them, while `i2c`
  returns `No I2C devices found`. **Never gate behaviour on them.**
- **`battery_pct` is permanently `1`; `charging` is `false`; the I²C storm is gone.**
  No PMU fitted. `getBattery()`/`isCharging()` now early-return on a stored `pmu_present`
  flag (`interface.cpp:62-74`, fixed in `b1c825c8`, ISSUE-3 partial), so `isCharging()`
  reports `false` and the old `ESP_ERR_INVALID_STATE` console stream is gone. `battery_pct`
  is still `1` — a sentinel was rejected because the value is assigned to a `uint8_t`, so
  `-1` would render as 255%; an honest "no battery" needs a cross-board `batteryPresent()`.
- No SD card: `SD` totals read `0 B`. Files live on LittleFS (~11.4 MB).

Ground truth comes from `i2c` (bus scan) and the `free` verb — not from
`capabilities`.

**`rf selftest` does not exist in this build.** This file previously recommended it as
ground truth for CC1101; the device answers `ERROR: Command not found at 'rf selftest'`
(verified 2026-07-29). Its registration sits inside `#if RF_DEBUG`
(`rf_commands.cpp:390-398`, called at `:449-453`), and `RF_DEBUG` defaults to 0
(`src/modules/rf/protocols/rf_config.h:19-21`) and is not overridden for
`smoochiee-board`. The same guard hides `keeloqtest` and `keeloqfiletest`. Rebuild
with `-DRF_DEBUG=1` if you want them.

## Testing

- **Host-side unit tests** (`pio test -e native`, `test/test_byte_ring/`) work only
  for pure logic with no Arduino/FreeRTOS/hardware dependency. `ByteRing` qualifies;
  almost nothing else currently does. Prefer extracting pure logic so it can be
  tested here.
- **Bench scripts** (`tools/ble_spike/`) exercise real transport semantics from a
  Linux laptop over BlueZ, no app build required: `spike_transport.py`,
  `spike_events.py`, `spike_swap.py`, `spike_suspend.py`, `probe_verbs.py`,
  `heap_poll.py`. Needs `pip install bleak`. `probe_verbs.py` is deliberately
  partial — it excludes destructive, blocking and menu-opening verbs, and that
  exclusion list should stay excluded from any blind sweep.
- **A panic also writes a full ELF core dump to flash, and `crashlog` reads it back.**
  `custom_16Mb.csv` has a 64 KB `coredump` partition and the framework sdkconfig sets
  `ESP_COREDUMP_ENABLE_TO_FLASH` + `DATA_FORMAT_ELF` + `CHECK_BOOT` — this was true from
  the fork's first commit and nothing read it until `0f4936d1`. `crashlog` over BLE
  reports the faulting task, `exc_pc`, the on-device Xtensa backtrace, the stored dump's
  `app_elf_sha256`, **and** the running firmware's own sha with a `match=` verdict, so a
  decode can be validated without a console attached. `crashlog -clear` erases it.
  Caveats: a *wedge* panics nothing and writes no dump (so this does not help ISSUE-30);
  `CAPTURE_DRAM` is off, so stacks and registers only; and the partition is overwritten
  by the next crash, so read it before deliberately provoking another.
- **Crash-testing a verb**: dispatch it over BLE while capturing `/dev/ttyACM0`
  (115200 raw). A panic prints `assert failed` / `Backtrace:` there — and, since
  `0f4936d1`, also survives in the core dump above, so a missed capture is no longer
  fatal to the investigation. Decode with:

  ```sh
  ~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
    -pfiaC -e .pio/build/smoochiee-board/firmware.elf <addresses>
  ```

  Check the panic's `ELF file SHA256` against the local ELF before trusting a decode.

- **The build is bit-for-bit reproducible, so an old dump is still decodable.** Measured
  2026-07-30: nine commits were rebuilt and every one reproduced the ELF hash recorded for
  it at the time. `esp_app_desc`'s embedded timestamp is the **framework's** build date
  (`Jul 15 2026`), not the local compile time, so nothing drifts between builds of the same
  source. Consequence: `git checkout <commit> && pio run -e smoochiee-board` regenerates the
  exact ELF a stored core dump needs, which is what makes `crashlog` useful days after the
  fact rather than only for the running build.

  **`sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9` is the panic handler's
  `ELF file SHA256` and `crashlog`'s `elf=`/`running_elf=`** — one digest, three places.
  Verified: all three read `79fc138cd` for `2b39286a`.

  | Commit | ELF sha256 (first 16) |
  |---|---|
  | `2d9422ea` | `5186685c0fdf19c2` |
  | `881ade6f` | `b919402708ab4de0` |
  | `4c4378a1` | `76d42c72f2b4a8a4` |
  | `22ab5974` | `411d7e151dbc2356` |
  | `cedad77f`, `038c00fd` | `f5244eb35dd10795` (identical — the later commit is docs-only) |
  | `6fd2b5fc` | `e81b0c28f80e70dd` |
  | `f54bbc6c` | `46d975be7d38f128` |
  | `afa17b57` | `2efaeec784a5768a` |
  | `3629afd7` | `3dd17e72827f4325` |
  | `253860a0` | `8a30e3153a15326a` (RC3; flashed and verified on hardware 2026-07-31) |

  `690cf842` differs from `253860a0` only under `test/`, which `[env:smoochiee-board]`
  does not compile, so it should produce the same ELF — **not rebuilt to confirm**, so
  treat that as inference, not measurement. Docs-only commits after `253860a0` likewise
  do not change it.

  ⚠️ **`TEST_STATUS.md` credits `76d42c72f2b4a8a4` to firmware "`fbfe6226`". That is wrong** —
  the commit that produces it is **`4c4378a1`**, confirmed by rebuild. Treat any
  commit-to-ELF attribution not in the table above as unverified.

  ⚠️ **Reproducible per *source*, not per *behaviour*: even a comment-only edit changes the
  ELF hash.** `log_e` bakes `__LINE__` into the binary — the console prints
  `[E][crash_commands.cpp:93]` — so inserting a comment above it shifts the hash without
  changing what the firmware does. Measured: adding one comment block took `2b39286a`'s
  `79fc138cd` to `fabcc0003`. So a `match=NO` from `crashlog` means "not this exact source",
  which is stricter than "not this code" — and it is the behaviour you want from a guard.

## Known gotchas

- **`[CLI] Result: TRUE` does not mean success.** **Narrowed 2026-07-31:** the *five
  interactive* verbs (`karma`, `deauth`, `blesniffer`, `ap_info`, `pwngrid`) still end
  in a bare `return true` (`runInteractiveAttack`, `attack_commands.cpp:212`) because
  the upstream entry points they call are `void`, so there is no outcome to propagate.
  Two verbs now report real failure: `deauth` returns `false` on an unsupported target
  (`:230`) and `reverseshell` returns `false` when its AP does not come up (`:249`).
  For any outcome decision prefer the `attack_result` frame (`pushAttackResult`,
  `:197-204`); `[CLI] Result:` is a *completion* signal only. Observed early on:
  `reverseshell` reported `TRUE` 30 ms after its AP creation had failed outright.
- **`deauth` and `evilportal`-under-load crashed the device, and are now mitigated —
  but ISSUE-1 is still open, on purpose.** Menu-dispatcher verbs draw to the TFT from the
  serial task while the main loop draws the same display over the same SPI bus; the bus
  mutex ended up released by the wrong task and FreeRTOS asserted. **The trigger is any
  sustained drawing from the serial task, not `drawArc` specifically** — `evilportal`
  died through `drawStatusBar`/`drawBatteryStatus`, the same status bar the main loop
  repaints on its 30 s timer. **Therefore an idle test proves nothing**, and every
  "survivor" in the seven-verb sweep was tested idle for 90 s.
  **`2d9422ea` re-scoped `tftMutex` from the call to the SPI transaction** and took
  `deauth` from crashing 2/2 in 20–70 s to **21 min clean across ~40 main-loop redraws**
  (ELF `5186685c0fdf19c2`); `blesniffer` went 13 min. The entry stays open because that
  is a probabilistic result against a race, and because blocking `evilportal` under load
  was never re-confirmed. **Do not read "ISSUE-1 open" as "no fix exists"** — a 2026-07-30
  audit made exactly that mistake, because the docs described the mechanism without ever
  naming `tftMutex`. Both backtraces and the full mitigation record are in
  `docs/KNOWN_ISSUES.md` §ISSUE-1.
- **An on-device chord is not a reliable way to exit a blocking verb.** The main loop keeps
  running the menu UI while a verb holds the serial task, so a physical press drives *both*
  it and the verb's own `check()` — during the ISSUE-39 test the operator's LEFT+RIGHT was
  consumed by the main menu (`loopOptions(): Selected: WiFi` in the console) instead of
  `reverseshell`. Same race as ISSUE-19, one module along.
- **On a dimmed screen the first button press is swallowed.** `InputHandler()` returns early
  when `wakeUpScreen()` reports it woke the display (`interface.cpp:119-123`), so `EscPress`
  is never set. Any "press LEFT+RIGHT to exit" instruction needs **two** presses. This has
  probably been misread as "the verb ignored Esc" more than once.
- **A blocking verb cannot be rescued over BLE**, only over HTTP `/cm nav esc`. And
  the dismissal key differs per verb: `ap_info` exits on **SELECT only** and ignores
  Esc; menu verbs take Esc or a "Main Menu" entry; `evilportal` takes Esc *then*
  "Exit Portal".
- **A verb that binds port 80 has no remote rescue at all.** `reverseshell` runs its own
  AsyncWebServer on 80, so the WebUI and its `/cm nav esc` are gone for the verb's whole
  life — the on-device LEFT+RIGHT chord is the only exit. Verifying anything about its
  exit therefore needs an operator at the board; budget for that rather than discovering
  it mid-test.
- **Radio verbs that exit cleanly may still leave their AP on air.** `EvilPortal` did
  (ISSUE-31) and `reverseshell` still does (ISSUE-39): its exit stops the TCP, WS, HTTP
  and DNS servers but never calls `WiFi.softAPdisconnect()`/`wifiDisconnect()`, so the AP
  keeps broadcasting and ~63 KB stays held after the verb returns. **`free` returning to
  its idle plateau is the cheap check** — 18,387 versus 81,327 is the whole tell.
- **`wifi_mode` in `/systeminfo` is an integer** (`0`=OFF `1`=STA `2`=AP `3`=APSTA),
  not a string. **`ip` is `WiFi.localIP()`**, which reads `0.0.0.0` in AP mode — use
  the known `192.168.4.1`.
- **`POST /cm` is POST-only** (GET 404s) and **never returns command output**; it
  returns `"command <verb> queued"`. Output goes to the event stream. Use the BLE CLI
  characteristic when you need actual reply text.
- **`/getscreen` is an HTTP route with no `addCommand` registration.** Sending
  `getscreen` through the CLI reaches a device with no such verb.
- **TFT draw logging is off unless the WebUI is running.** `display dump` therefore
  returns only a lone `SCREEN_INFO` packet over BLE. `display start` would enable it
  but streams *raw binary* draw packets onto the CLI characteristic, corrupting EOT
  framing.
- **`Serial` is the native USB-CDC port and reaches nothing on this board.** The bench
  reads the **UART bridge**, which carries `ESP_LOG` and panic output — not `Serial`
  (`ram_profile.cpp:9-18`). `RAM_LOG` reaches the console only because it explicitly
  mirrors to UART0 TX GPIO43 (`ram_profile.cpp:19-32`). **`log_e` is the only compiled
  log level** (`-DCORE_DEBUG_LEVEL=1`, `boards/smoochiee-board/smoochiee-board.ini:21`),
  so `log_i`/`log_w` are stripped and are not an option. This one fact is the root cause
  behind ISSUE-2, ISSUE-22, ISSUE-42 **and** ISSUE-19's stalled attribution — a
  diagnostic written with `Serial.printf` is not weak evidence when it fails to appear,
  it is *no* evidence, because it was never addressed to the console being watched.
  Anything the app or the bench must read goes to `serialDevice->` (CLI reply),
  `log_e` (console) or `pushWsLog` (event stream).
- **Line numbers in docs drift.** Citations were correct when written; grep for the
  symbol if a line does not match.

## Further reading

Docs under `docs/`, in the order worth reading them:

- **`docs/TEST_STATUS.md`** — coverage map: what has actually been run on hardware,
  what is shippable, what is broken, and what is still untested with the reason.
  **Start here** if you are deciding what the app can offer.
- **`docs/KNOWN_ISSUES.md`** — verified defect register. **Read before planning
  against any verb.** VERIFIED vs SUSPECTED is tracked explicitly; fixed entries move
  to §Resolved with the commit and the proving test rather than being deleted.
- **`docs/bruce-companion-api.md`** — the interface contract. What the app can call,
  what each verb costs, what telemetry exists.
- **`docs/FIRMWARE_CHANGES.md`** — rationale. Why the fork exists, what changed by
  area with commit refs, and the dead ends (documented because the code still carries
  guards that only make sense if you know them).

`LAYOUT.md` and `2.0_road_path.md` are upstream files, not fork documentation.
