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
- **Companion app:** `/home/kevsmir07/Projects/maritest` (React Native/Expo). It
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
check fails its fallback tears down WiFi to make room. With the BLE API up it is
therefore **the memory guard, not the attack**, that kills the AP.

Consequences that shape most of the code:

- Free heap does not predict this. Use the `free` verb, which reports the largest
  contiguous DMA block explicitly.
- **"WiFi as bulk transfer" is aspirational, not current behaviour.** Measured
  2026-07-29: with the BLE API armed, `webui` cannot start at all — AsyncTCP fails to
  allocate its task with 1,235 bytes free, the HTTP server never listens, and the AP
  beacons but cannot accept a station. BLE replies truncate to 1 byte in the same
  state. The two transports are not merely tight together, they are mutually
  exclusive today. See `docs/KNOWN_ISSUES.md` §ISSUE-12.
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
  That is a real bug in `settings_commands.cpp:19` (see `docs/KNOWN_ISSUES.md`).
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

Advertises as `Bruc`. Service `4371ec0b-…`, CLI characteristic `d555ed97-…`
(READ|NOTIFY|WRITE), event characteristic `d555ed98-…` (READ|NOTIFY).

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

Frames actually emitted: `state`, `log`, `ble_progress`, `ble_result`. Nothing else,
whatever older design notes claim.

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
- **`battery_pct` is permanently `1` and `charging` permanently `true`** — no PMU
  fitted, and `getBattery()` calls `PPM.*` regardless of whether `PPM.init()`
  succeeded. Expect a continuous `ESP_ERR_INVALID_STATE` I2C error stream on the
  console; it is this, and it is not a new fault.
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
- **Crash-testing a verb**: dispatch it over BLE while capturing `/dev/ttyACM0`
  (115200 raw). A panic prints `assert failed` / `Backtrace:` there and nowhere else.
  Decode with:

  ```sh
  ~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
    -pfiaC -e .pio/build/smoochiee-board/firmware.elf <addresses>
  ```

  Check the panic's `ELF file SHA256` against the local ELF before trusting a decode.

## Known gotchas

- **`[CLI] Result: TRUE` does not mean success.** Every attack callback ends in a bare
  `return true` and discards the outcome (`attack_commands.cpp:147-175`). Observed:
  `reverseshell` reported `TRUE` 30 ms after its AP creation failed outright. It is a
  *completion* signal only.
- **`deauth` crashes the device.** Menu-dispatcher verbs draw to the TFT from the
  serial task while the main loop draws the same display over the same SPI bus; the
  bus mutex ends up released by the wrong task and FreeRTOS asserts. Only `deauth`
  reproduces so far — it drives `drawArc` continuously, while the survivors redraw
  only on input. Full backtrace in `docs/KNOWN_ISSUES.md` §ISSUE-1.
- **A blocking verb cannot be rescued over BLE**, only over HTTP `/cm nav esc`. And
  the dismissal key differs per verb: `ap_info` exits on **SELECT only** and ignores
  Esc; menu verbs take Esc or a "Main Menu" entry; `evilportal` takes Esc *then*
  "Exit Portal".
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
