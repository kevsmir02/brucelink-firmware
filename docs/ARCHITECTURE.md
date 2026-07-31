# Architecture

How this fork turns a screen-and-buttons device into one a phone can drive. Companion to
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) (why each change was made) and
[bruce-companion-api.md](./bruce-companion-api.md) (the wire contract).

Verified against the tree at HEAD `26210f95`, 2026-08-01.

---

## 1. The constraint that shapes everything

Every Wi-Fi attack in Bruce tears down the Wi-Fi stack. So the obvious remote-control
path — the existing Web UI over HTTP — destroys itself the moment you use it for the thing
you wanted it for. **BLE is therefore the primary control transport**, with Wi-Fi as bulk
transfer only.

But BLE and Wi-Fi compete for one scarce resource: internal DMA-capable RAM. PSRAM
**cannot** back the BT controller. `radioHasMemForBle()` (`src/core/radio_mem.h:39`) admits
a BLE bring-up only when the largest **contiguous** DMA block clears
`RADIO_BLE_MIN_DMA_BLOCK` = 15 KB (`radio_mem.h:32`); Wi-Fi has the same threshold
(`radio_mem.h:29`). When the BLE check fails, the fallback tears down Wi-Fi to make room
(`radio_mem.h:52`).

Consequences that explain most of the code:

- **Free heap does not predict this.** Use the `free` verb, which reports the largest
  contiguous DMA block explicitly (`radioLargestDmaBlock()`, `radio_mem.h:25`, wrapping
  `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)`).
- **The two transports coexist, but with almost no margin.** From a fresh boot, `webui`
  starts and BLE answers with the AP up. From a boot where anything already took ~18 KB,
  the listener fails to allocate and the AP beacons but cannot accept a station. See
  [KNOWN_ISSUES §ISSUE-12](./KNOWN_ISSUES.md).
- **Never let both radios be loaded at once.** `blespam` suspends the BLE API *before*
  touching NimBLE, which frees ~62 KB and takes the DMA block from ~1.3 KB to ~32 KB.
- **Ordering is everything.** Tearing BLE down cleanly *before* an attack means the rebuild
  is a fresh `setup()`. Rebuilding *after* an attack has already mangled the stack was
  tried and reverted — it crashed (`eb05177b` → `e2631370`).

---

## 2. The command bus

One CLI (`SerialCli::parse`, SimpleCLI) is the single command surface. **Every verb works
identically over either transport** — there is no BLE-only or HTTP-only verb.

Verbs are registered per area in `src/core/serial_commands/*.cpp`, each exposing a
`createXCommands(SimpleCLI*)` called from `cli.cpp:41-64`. `attack_commands.cpp` and
`crash_commands.cpp` are this fork's files; the rest are upstream.

### `serialDevice` rebinds at runtime

`serialDevice` is a global `SerialDevice*`: `&USBserial` normally, `&serial_service` (the
BLE GATT service) while the BLE API is armed (`ble_api.cpp:63`, restored at `:110`). This
is the single most common source of confusion in this codebase:

- Output written to `Serial` instead of `*serialDevice` **never reaches the app**. This was
  the root cause of ISSUE-2 and ISSUE-42. Anything the app must read goes through
  `*serialDevice`.
- While BLE is armed, the USB CDC port accepts **no** CLI input. It still carries
  `ESP_LOG` and panic output, which is why the console is the only place a backtrace
  appears live (ISSUE-22).

---

## 3. Dispatch, and the blocking problem

`handleSerialCommands()` (`src/core/serialcmds.cpp`) runs on its own FreeRTOS task,
`_serialCmdsTaskLoop` (`:97`, created at `:111`), and calls `serialCli.parse()` **inline**
(`:67`). Two entry paths reach it:

1. **HTTP** `POST /cm` → a depth-2 queue (`xQueueCreate(2, …)`, `:107`) → drained by the
   serial task.
2. **BLE** → bytes land in a 512-byte ring via `pushRx` on the **NimBLE host task**
   (`BLE_RX_RING_SIZE`, `BLESerialService.h:11`) and the serial task drains complete lines.

Because `parse()` is inline, a verb that opens a TFT menu holds the serial task for its
entire life. While blocked, **no BLE command is parsed at all** — the bytes arrive and sit
unread. The one exception is HTTP `/cm cmnd=nav …`, special-cased in the AsyncWebServer
task *before* queueing, writing the button globals directly
(`webInterface.cpp:545-555`) — which is why the only remote rescue for a blocked verb is
`POST /cm cmnd=nav esc`, and never BLE.

`COMMAND: <verb>` is pushed to the event stream **before** `parse()` (`serialcmds.cpp:65`)
and `[CLI] Result:` **after** (`:72`), so a dispatch ACK reaches the app even for a verb
that then blocks for minutes.

> **`[CLI] Result: TRUE` does not mean success.** The five interactive verbs (`karma`,
> `deauth`, `blesniffer`, `ap_info`, `pwngrid`) end in a bare success return
> (`runInteractiveAttack`, `attack_commands.cpp:206-212`) because the upstream entry points
> they call are `void` — there is no outcome to propagate. Two verbs do report real
> failure: `deauth` on an unsupported target (`:229`) and `reverseshell` when its AP does
> not come up (`:248`). For any outcome decision prefer the `attack_result` frame;
> `[CLI] Result:` is a *completion* signal only.

---

## 4. BLE transport — `src/modules/ble_api/`

Upstream shipped a BLE API; this fork made it a reliable command bus (framing, a race-free
RX ring, MTU-aware chunking, clean teardown). The device advertises as `Bruc`.

| | UUID | Properties |
|---|---|---|
| Service | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` | — |
| CLI characteristic | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` | READ \| NOTIFY \| **WRITE** |
| Event characteristic | `d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9` | READ \| NOTIFY |

Source of truth: `BLESerialService.cpp:66,70,80`. The three UUIDs share only their first
field — writing them abbreviated once produced two characteristic UUIDs that do not exist
on the device, so they are always written out in full.

The CLI characteristic does **not** advertise `WRITE_NR`, so a client must write **with**
response — `bleak`'s `response=False` is rejected against this GATT table.

- **Commands** are newline-terminated. **Responses** end with a `0x04` EOT byte
  (`BLE_RESPONSE_EOT`, `BLESerialService.h:22`) — *not* a `"# "` prompt, because any output
  line beginning with `# ` (a dumped markdown file, a commented config) would truncate the
  reply and desynchronise every command after it.
- Notifies are chunked to `negotiated_MTU - 3` (`BLESerialService.cpp:150`). The client
  **must** request MTU 247; Android's default 23 yields 20-byte chunks.
- `ByteRing` (`services/ByteRing.h`) is the RX FIFO, guarded by a FreeRTOS mutex because
  `pushRx` (NimBLE task) and reads (serial task) genuinely race. It is the one piece with
  host-side unit tests.
- Events go on their **own** characteristic so async JSON never interleaves with CLI
  stdout. Events are dropped, not queued, when nobody is subscribed.
- Teardown must use `NimBLEDevice::deinit(true)`. The default leaves services allocated,
  producing duplicate characteristics with the same UUID on every arm/disarm cycle — and it
  also keeps the advertising payload, which is how `blespam` used to come back nameless
  (`c9c43c03`). Anything handing the radio back must leave the advert empty *and* restore
  the BT MAC it rotated.

---

## 5. HTTP transport

The upstream Web UI, kept for bulk transfer (file listing, screenshots) and as the only
remote rescue path.

- `webui` starts it, `webui -off` frees it and its AP without touching BLE
  (`wifi_commands.cpp:55-134`). The start is **gated**: since RC3 it reports a real outcome
  rather than always claiming success, so `[CLI] Result: FALSE` from `webui` means it is
  not serving. The gate's decision logic is pure and unit-tested (`webui_gate.h`,
  `test/test_webui_gate/`).
- **`POST /cm` is POST-only** (GET 404s) and **never returns command output** — it returns
  `"command <verb> queued"`. Output goes to the event stream. Use the BLE CLI
  characteristic when you need reply text.
- `/getscreen` is an HTTP route with **no** CLI registration; sending `getscreen` through
  the command bus reaches a device with no such verb.

---

## 6. Event stream — `src/core/wifi/ws_events.cpp`

`pushWsEvent()` (`ws_events.cpp:56`) builds one frame and fans it out to both `/ws` and the
BLE event characteristic. `wsEventId` increments **unconditionally** (`:15,62`), even with
nobody listening, so the ID space is gap-free across a transport switch and an app tracking
`lastEventId` can detect what it missed. There is **no** server-side replay; that was judged
not worth the RAM.

Frames actually emitted: `state`, `log`, `ble_progress`, `ble_result` and `attack_result`.
Nothing else, whatever older design notes claim.

`attack_result` carries the only *real* outcome signal the attack verbs have
(`pushAttackResult`, `attack_commands.cpp:197`):

```json
{"id":12,"type":"attack_result","verb":"ap_info","outcome":"completed",
 "elapsed_ms":109994,"wifi_mode":2,"free_heap":7699}
```

Captured live on the event characteristic, 2026-07-30.

---

## 7. Boot and persistence

`main.cpp:589` re-arms the BLE API at the **end** of `setup()`, after Wi-Fi init, if
`bruceConfig.bleApiAutoStart` is set. This is opt-in and deliberately runs last — it is
*not* the reverted `eb05177b`, which ran unconditionally *before* Wi-Fi to win the DMA race
and crashed.

Practical effect: **Config → Toggle BLE API** is a one-time action on the device. A flash
that wipes the config file resets it to off, and the board then has no command interface at
all until someone touches the screen.

---

## 8. Diagnostics

- **`RAM_LOG()` stage markers** are compiled in (`-D ENABLE_RAM_LOGGING=1`, `platformio.ini`
  `[env] build_src_flags`) and print a full heap profile at each subsystem transition. They
  reach the console only because they explicitly mirror to UART0 TX on GPIO 43
  (`ram_profile.cpp:16-25`) — `Serial` is the native USB-CDC port and does not reach the
  UART bridge the bench reads.
- **The periodic sampler is separately opt-in** (`-D ENABLE_RAM_SAMPLER=1`) because its
  4 KB task stack comes out of internal DRAM — larger than the margin it would be
  measuring. A diagnostic that changes the result is worse than no diagnostic.
- **A panic writes a full ELF core dump to flash**, and the `crashlog` verb reads it back
  over BLE — faulting task, `exc_pc`, on-device backtrace, the stored dump's
  `app_elf_sha256` **and** the running firmware's own hash with a `match=` verdict. See
  [BUILDING.md §Decoding a crash](./BUILDING.md#decoding-a-crash).

---

## 9. What is new versus what was modified

`git diff --shortstat 59e83bfb..26210f95` — 98 files changed, 14,962 insertions,
371 deletions, across 124 commits since `373fb5d8`.

The fork stays mergeable by preferring **new files** over edits scattered through upstream
code:

| New | Purpose |
|---|---|
| `src/core/serial_commands/attack_commands.cpp` | The remote-callable attack verbs |
| `src/core/serial_commands/crash_commands.cpp` | `crashlog` — reads the stored core dump |
| `src/core/system_info.cpp` | Extended `/systeminfo` telemetry |
| `src/core/wifi/ws_events.cpp` | The unified event stream |
| `src/core/wifi/webui_gate.h` | Pure decision logic for the WebUI start gate (unit-tested) |
| `src/modules/wifi/evil_portal_bg.cpp` | Headless Evil Portal (`-bg`/`-off`/`-status`/`-duration`) |
| `src/modules/wifi/portal_cap.h` | Pure portal duration/cap logic (unit-tested) |
| `src/modules/ble_api/services/ByteRing.h` | Mutex-guarded RX FIFO (unit-tested) |
| `src/core/crash_report.h` | Pure core-dump rendering (unit-tested) |

Modified upstream files, kept to the minimum: `ble_api.cpp/.hpp`,
`BLESerialService.cpp/.h`, `BatteryService.cpp`, `serialcmds.cpp`, `webInterface.cpp/.h`,
`wifi_common.cpp`, `main.cpp`, `include/SerialDevice.h`, and
`boards/smoochiee-board/interface.cpp`.
