# What this fork changes in the Bruce firmware

Companion doc to [bruce-companion-api.md](./bruce-companion-api.md). That file is
the **interface** — what the app can call. This one is the **rationale** — what was
changed inside the firmware, why, and what it cost. Verified defects live in
[KNOWN_ISSUES.md](./KNOWN_ISSUES.md).

> **The menu-dispatcher verbs added by this fork crash the device.** `deauth` panics
> reproducibly with a FreeRTOS assert — the verbs draw to the TFT from the serialcmds
> task while the main loop draws the same display, and the SPI bus mutex is released
> by the wrong task. This is the single most significant thing found since the fork
> was written up. See KNOWN_ISSUES §ISSUE-1.

**Baseline:** upstream Bruce at `59e83bfb` (2026-07-23).
**This fork's work starts at:** `373fb5d8` (2026-07-25).
**Audited at:** `623d4d26` (2026-07-28).
**Tested on:** Smoochiee V2 (ESP32‑S3‑N16R8 — 16 MB flash, 8 MB OPI PSRAM), env
`smoochiee-board`. Nothing here has been run on any other board. See §7.

Diff size: 4,050 insertions / 109 deletions across 44 files — `git diff --shortstat
59e83bfb..623d4d26`, i.e. measured at the audit commit above. The same command
against a later HEAD reports less, because `0b2073fa` deleted `docs/superpowers/`.
Quote the endpoint with the number.

Commit breakdown, `373fb5d8^..0b2073fa`: 51 commits — 19 `fix`, 17 `feat`, 7
`tools`, 6 `docs`, 1 `test`, 1 `refactor`. 39 touch `src/` or `include/`; 12 are
docs or tooling only.

---

## 1. The problem this fork solves

Bruce is driven from its screen and buttons. The companion app needs to drive it
remotely — and the moment you try, you hit a wall that is not a software design
choice but a **hardware constraint**:

> Every Wi‑Fi attack in Bruce tears down the Wi‑Fi stack. So the obvious remote
> control path — the existing Web UI over HTTP — destroys itself the instant you
> use it for the thing you wanted it for.

BLE survives Wi‑Fi attacks, so BLE became the control path. But BLE and Wi‑Fi
fight over the same scarce resource (internal DMA-capable RAM — PSRAM cannot back
the BT controller), so most of the work below is about **never having both radios
loaded at once**, and swapping cleanly between them.

Everything else is consequence: the transport had to be made reliable enough to
carry a command bus, the teardown paths had to stop crashing, and the device had
to report enough state for an app to know what it was doing.

---

## 2. Changes by area

### 2.1 Remote command surface

| Change | Commits |
|---|---|
| `attack_commands.cpp` — new CLI verbs (`ble api`, `evilportal`, `blespam`, `karma`, `deauth`, `blesniffer`, `ap_info`, `reverseshell`, `pwngrid`) so attacks are reachable from the command bus, not just the menu | `32b1dcf6`, `500dcefa`, `8e8b587c`, `02568b56`, `0418d39e` |
| `blespam` wired to the generic spam engine (apple/android/ibeacon/samsung/windows/random) instead of FastPair only | `de2c79e1` |
| `/systeminfo` extended with capability flags, battery and network telemetry, so the app can grey out buttons for hardware that isn't there | `ed8f6806` |
| `buildSystemInfoJson()` extracted + `systeminfo` CLI verb, so the same JSON is readable over BLE and not just HTTP | `8fdfb28a` |
| `free` verb rewritten to report the **largest contiguous DMA block**, not just free heap | `2dfcda8f` |
| `webui -off` / `webui -bg` so a remote client can stop the Wi‑Fi stack to free memory and bring it back without walking to the device | `9e389f4f` |
| Bearer-token auth accepted alongside the session cookie | `9563fa36` |

**Design note on `/cm`:** it queues and returns `"queued"` — it never returns
command output. That is upstream behaviour and was left alone. It is the reason
the BLE CLI characteristic exists as a separate request/response path rather than
the app just POSTing everything.

### 2.2 Event stream

Upstream had no way to tell a remote client what the device was doing.

- `/ws` AsyncWebSocket + `pushWsEvent()` (`f3eeb0dd`)
- BLE attack progress/result pushed as events (`59876846`)
- CLI dispatch and result forwarded as `log` frames; `state` frames around attacks (`c3dd813c`)
- Event frames split onto their **own BLE characteristic** with EOT framing, so
  async JSON never interleaves with CLI stdout on one byte stream (`9c7f7260`)

`wsEventId` increments even when nobody is listening, so the ID space is gap-free
across a transport switch — the app can tell it missed something. There is no
server-side replay; adding one was judged not worth the RAM on this board.

### 2.3 BLE transport reliability

The BLE serial link started as a toy and had to become a command bus. Each of
these was a real observed failure, not hardening for its own sake:

| Fix | Symptom it fixed | Commit |
|---|---|---|
| `ByteRing` FIFO for RX (unit-tested) | back-to-back commands dropped | `a45de9d8`, `c05b8c9e` |
| FreeRTOS mutex around the ring | RX corruption — `pushRx` runs on the NimBLE host task, reads happen on the loop task | `d85d126a` |
| Chunk notifies to the negotiated MTU | replies over ~20 bytes silently lost; also fixed UB in `vprintf` | `fc18234e` |
| Newline framing on commands | a command split across two writes parsed as a fragment | `a512e3c1` |
| Larger battery-task stack, idempotent `BatteryService::end` | crash on teardown | `dd2ef38d` |
| Detach characteristic callbacks before freeing | use-after-free on teardown | `6867bd7a` |
| Clear event subscription on peer disconnect | a vanished peer never writes 0 to the CCCD, so every event burned 8 notify retries per chunk, forever | `d092849c` |
| `NimBLEDevice::deinit(true)` on teardown | default deinit leaves services allocated → duplicate characteristics with the same UUID after each arm/disarm cycle (measured: 2, then 3), advertising data overflow, clients unable to write | `ee9bf54c` |
| Restore `BLE_OWN_ADDR_PUBLIC` after FastPair | an attack that rotated MACs left a static flag set; advertising then failed `rc=530` and the control link never came back | `081d8114` |
| Report truncation in-band | a memory-starved device returned silently truncated JSON and the app saw a parse error instead of "the device ran out of RAM" | `16208a6b` |

**Why EOT (`0x04`) and not the `"# "` prompt:** any CLI output line that begins
with `# ` — a dumped markdown file, a commented config, a script listing — would
truncate the response and desynchronise every command after it.

### 2.4 Radio coexistence — the core of the fork

This went through three attempts. The dead ends are documented because the code
still carries guards that only make sense if you know them:

1. **`eb05177b` — auto-start BLE before Wi‑Fi init.** Win the DMA race by getting
   there first. **Reverted (`e2631370`):** crashed. Bypassing `radioHasMemForBle()`
   half-initialises `esp_bt_controller_init` and panics.
2. **`302fd170` / `2472a7e5` — let the guard drop the AP, then restart it.**
   Worked, but the app lost Wi‑Fi mid-attack and had to reconnect.
3. **`39f7c8bd` — suspend the BLE API *before* the attack touches NimBLE.**
   Current design. Freeing the BLE stack while it is still healthy releases ~62 KB
   and takes the largest contiguous DMA block from **1,332 bytes → 32,756 bytes**,
   so the memory guard passes on its first check and never touches Wi‑Fi.
   Those two figures were measured 2026-07-27 with a station associated to the AP;
   a re-measurement on 2026-07-29 with no station associated read 6,900 bytes
   loaded and 31,732 bytes BLE-only. Both are far below and far above the 15 KB
   guard respectively, so the design holds either way — but see the API contract
   §7.1 before quoting a specific number.

The insight that made it work: with the BLE API up, it is **the memory guard, not
the attack**, that destroys the AP. Ordering is everything — attempt 1 rebuilt BLE
*after* an attack had mangled the stack; attempt 3 tears it down cleanly *before*,
so the rebuild is a fresh `setup()` rather than a rescue.

The AP-restore path from attempt 2 is still present as a fallback. If you ever see
`[BLE_SPAM] Restoring WiFi AP` in the log, the swap did not free enough memory —
that is a signal worth investigating, not ignoring.

`bleApiSuspend()`/`bleApiResume()` deliberately do **not** touch the persisted
enable flag: a swap around an attack is not the user changing their mind.

### 2.5 Boot persistence

`623d4d26` persists `bleApiAutoStart` in the config file and re-arms the GATT
server at the **end** of `setup()`, after Wi‑Fi. Before this, every reboot needed a
walk to the device, or a Wi‑Fi session opened purely to send `ble api on`.

This is not a re-do of the reverted `eb05177b`. That one ran unconditionally and
*before* Wi‑Fi init to win the DMA race. This one is opt-in, runs last, and only
works because §2.4 guarantees the two radios are never loaded simultaneously.

### 2.6 Crash and UX fixes found along the way

These are upstream bugs this fork hit while building the above. They are not
companion-app features, and are the most likely part of this work to be useful to
anyone else:

- **`d450faf5` — WebUI teardown crashed the device.** `AsyncWebServer` takes
  ownership of handlers it is given (`std::list<std::unique_ptr<>>`), so a
  file-static WebSocket made `stopWebUi()` call `free()` on a `.bss` address:
  *"free() target pointer is outside heap areas"*, reboot, every time a Wi‑Fi
  attack tore the WebUI down. Also required draining the client list before the
  destructor runs — the AsyncTCP task services disconnects concurrently and
  `~AsyncWebSocket()` takes no lock, giving a double free
  (*"assert failed: multi_heap_free (head != NULL)"*).
- **`7de5b1b9` — Evil Portal served no pages.** Port 80 was not bound with
  `SO_REUSEADDR`, so the portal could not take the port after the WebUI released it.
- **`f969bee9` — FastPair popup spam crashed the device.**
- **`7c1c2ce7` — the device looked frozen after a remote command.** A CLI command
  draws over the screen, then calls `backToMenu()`. Everywhere else that flag is
  consumed by the caller's own loop — but the main menu is the top of the stack,
  so there was no caller to unwind into and the request was dropped. The last
  frame stayed on screen until a key was pressed. It read as a hang (Evil Portal
  appearing stuck on *"Shutting down…"*) while the firmware was running fine and
  still answering commands.

### 2.7 Diagnostics and tooling

- `RAM_LOG()` stage markers, mirrored to UART0 — on this board `Serial` is the
  native USB‑CDC port, which is usually not plugged in, so ordinary `printf`
  debugging reaches nobody (`55004e86`, `de15d9fa`).
- The periodic sampler is **separately opt-in** (`-D ENABLE_RAM_SAMPLER=1`). Its
  4 KB task stack comes out of internal DRAM — larger than the ~2.5 KB
  fully-loaded margin it would be measuring, and on its own enough to stop a
  station associating with the AP (`266542e7`). A diagnostic that changes the
  result is worse than no diagnostic.
- `tools/ble_spike/` — Linux/BlueZ bench scripts that exercise the same transport
  semantics the app will, so a firmware change is validated in ~30 seconds instead
  of an Android build cycle (`0ab152a7`, `a43eaf5b`, `167e7e3e`).
- `[env:native]` in `platformio.ini` + `test/test_byte_ring/` — host-side unit
  tests for the pure-logic pieces.

---

## 3. Files touched

```
src/core/serial_commands/attack_commands.{cpp,h}   new — the remote verb surface
src/core/wifi/ws_events.{cpp,h}                    new — event fan-out
src/core/system_info.{cpp,h}                       new — shared /systeminfo builder
src/modules/ble_api/services/ByteRing.h            new — RX FIFO
src/modules/ble_api/services/BLESerialService.*    transport: framing, chunking, events
src/modules/ble_api/ble_api.cpp                    lifecycle, event sink, teardown
src/core/settings.cpp                              bleApiSuspend/Resume + persistence
src/main.cpp                                       boot arming
src/core/wifi/webInterface.cpp                     bearer auth, /ws wiring, teardown fix
src/core/serialcmds.cpp                            line framing, EOT, log forwarding
src/core/display.cpp                               main-menu repaint fix
src/modules/ble/ble_spam.cpp, BLE_Suite.cpp        headless entry points, telemetry
src/core/ram_profile.{cpp,h}                       diagnostics
tools/ble_spike/                                   bench scripts
test/test_byte_ring/                               unit tests
```

---

## 4. Deliberate non-goals

Not built, and not planned unless the app actually needs them:

- **`cred` / `host` / `ap` / `packet` event frames.** Evil Portal credentials are
  polled from the portal's own endpoint instead (see the API doc §6).
- **Server-side event replay.** The app detects gaps via `lastEventId` but cannot
  request a re-send. Buffering costs RAM this board does not have.
- **Non-blocking menu-dispatcher verbs.** `karma`, `deauth`, `blesniffer`,
  `ap_info`, `reverseshell` and `pwngrid` still block the serial task until
  someone presses a button on the device.
  **Superseded 2026-07-29 — this is no longer merely a limitation.** `deauth`
  *crashes* the device (KNOWN_ISSUES §ISSUE-1): drawing the menu from the
  serialcmds task while the main loop draws the same TFT releases the SPI bus
  mutex from the wrong task and trips a FreeRTOS assert. Running them in a
  dedicated FreeRTOS task, as suggested here, would **not** fix that — it adds a
  third task to the same unarbitrated display. The fix is either to serialise all
  TFT access behind one owner, or to give these verbs headless entry points that
  never draw, the way `blespam` already works.
- **A headless `evilportal`.** It belongs in the list above: the CLI verb builds a
  stack-local `EvilPortal` with `backgroundMode=false`, so the constructor calls
  `loop()`, whose only exit is ESC → "Exit Portal" on the device. There is no
  duration check and no remote stop. A working headless path already exists —
  Karma heap-allocates the portal with `backgroundMode=true` and pumps
  `processRequests()` (`karma_attack.cpp:1786,1747`) — but the CLI verb does not
  use it, and simply flipping the flag would destroy the stack temporary. See the
  API contract §5.3.
- **`deauth <target>` targeting.** The argument is accepted and discarded; the
  verb opens the on-device menu.
- **Simultaneous BLE + Wi‑Fi as a steady state.** It works, but with ~15 KB free
  and a single associated station driving that to a few hundred bytes, it is a
  transition state only.

---

## 5. Known limits

- **`/cm` blocks on a depth‑2 queue.** While a blocking verb holds the serial
  task, all further `/cm` calls return HTTP 400.
- **Blocking verbs emit no `state` frame**, so "is it done yet" has no clean
  signal. They *do* emit a `COMMAND: <verb>` log frame before they start, because
  it is pushed ahead of `parse()` — that is a usable dispatch ACK, but the
  completion signal (`[CLI] Result:`) does not arrive until the verb exits. See
  the API contract §4.1.
- **`/systeminfo` capability flags do not describe the hardware.** They are
  compile-time `#if defined(...)` checks against the board profile.
  `has_pn532`/`has_fm`/`has_eth` are hardcoded `false`; the rest report whatever
  `boards/smoochiee-board/pins_arduino.h` declares. On a bare ESP32‑S3‑N16R8 with
  only an LCD and buttons, `/systeminfo` claims CC1101, NRF24, GPS, IR, buzzer,
  RGB LED and mic while `i2c` on the same device returns `No I2C devices found`
  (measured 2026-07-29). Runtime probing is not implemented.
- **`battery_pct` and `charging` are wrong with no PMU fitted.** `getBattery()`
  and `isCharging()` call `PPM.*` regardless of whether `PPM.init()` succeeded
  (`boards/smoochiee-board/interface.cpp:38-75`). On a board with no BQ25896 they
  report a permanent `battery_pct: 1` / `charging: true` and the failing polls
  emit a continuous `ESP_ERR_INVALID_STATE` stream on the console — roughly 10
  per 22 s, measured 2026-07-29.
- **iOS visibility unconfirmed.** `Bruc` is visible in BLE scans from a PC; an
  iPhone 8 did not see it, likely iOS BLE privacy filtering. Untested on other
  iOS devices.

---

## 6. Reproducing the measurements

Every number in this document and in the API contract came from hardware, not
estimation. To re-take them on your own board:

```sh
# 1. flash with RAM logging on
pio run -e smoochiee-board -t upload    # RAM_LOG markers are compiled in

# 2. watch the DMA block over UART0 while loading each subsystem
.venv/bin/python tools/ble_spike/heap_poll.py

# 3. verify the transport swap end to end
.venv/bin/python tools/ble_spike/spike_swap.py
.venv/bin/python tools/ble_spike/spike_suspend.py
```

`free` over BLE reports the same numbers on demand. If your board's largest
contiguous DMA block behaves differently from the figures in §2.4, the coexistence
design in this fork may not hold for it — see §7.

---

## 7. Hardware caveat

All of this was developed and tested on exactly one board: **Smoochiee V2
(ESP32‑S3‑N16R8)**. The radio coexistence work in §2.4 is tuned to the memory
profile of that specific chip and PSRAM configuration.

Boards with less internal DRAM, no PSRAM, or a different flash/PSRAM layout may
behave differently — most likely the suspend/resume swap will not free enough
contiguous DMA, and the fallback AP-restore path will fire (or the guard will drop
Wi‑Fi anyway). Upstream Bruce supports a long list of devices; **this fork makes no
claim about any of them.**

If you try it elsewhere, `free` and the `RAM_LOG` markers are the tools that will
tell you what is happening.
