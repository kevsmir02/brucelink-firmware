# Test status — what is actually known to work

Coverage map for the fork, as of **2026-07-30**. Companion to
[KNOWN_ISSUES.md](./KNOWN_ISSUES.md) (the defect detail and evidence),
[bruce-companion-api.md](./bruce-companion-api.md) (the interface contract) and
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) (why the fork exists).

**Why this file exists.** The other three answer "what is it" and "what is broken".
This one answers "what has anyone actually run on hardware", so the next person can
tell a verified capability from an assumed one at a glance. Every row is either a
measurement (device + date) or a code fact (`file:line`). Nothing here is inferred.

**Hardware under test:** bare ESP32‑S3‑N16R8 devkit, 1.47" 172×320 IPS LCD on SPI,
five buttons, USB powered. No PMU, SD, CC1101, NRF24, PN532, IR or GPS. Env
`smoochiee-board`. Firmware `2d9422ea`, ELF `5186685c0fdf19c2`; the headless-portal
rows below are from firmware `fbfe6226`, ELF `76d42c72f2b4a8a4`.

---

## The three rules that override everything

1. **An idle test proves nothing.** `evilportal` survived 100 s idle and then crashed
   under load with the same assertion as `deauth` (ISSUE-1). Every "survivor" in the
   seven-verb sweep was tested idle for 90 s. Their clean results establish nothing.
2. **A compile-time `#define` is not a capability.** `USB_as_HID` is defined and
   BadUSB still cannot work (ISSUE-20); `/systeminfo` reports seven devices that are
   not fitted (ISSUE-4); `rf selftest` is documented but not compiled in.
3. **Silence is not success.** Several verbs return an empty reply on success
   (`gpio`), several return an empty reply on failure (`md5`, `stat`, `decrypt`,
   `settings`), and `[CLI] Result: TRUE` is hardcoded for every attack (ISSUE-7).

---

## Shippable today

Verified end-to-end on hardware, safe to expose as a one-tap action.

| Capability | Evidence |
|---|---|
| **Evil Portal (BLE off)** | Serves the page, answers Android `/generate_204` and iOS `/hotspot-detect.html`, captures credentials, returns them at `/creds`. Verified 2026-07-29. |
| **Evil Portal headless — `evilportal -bg`** | The verb no longer holds the serial task: `uptime` over BLE answered in **0.06 s** during a live portal, `-status` answered 9× during one. Duration cap self-stopped at **+45.6 s** on a 45 s cap, with **zero** stray bytes on the CLI characteristic. AP confirmed on air by an independent `nmcli` scan. ELF `76d42c72f2b4a8a4`, 2026-07-30. **Serving the page still needs `ble api off`** (ISSUE-21). |
| `evilportal -off` / `-status` | `-off` with nothing running returns `no background portal running`, not a false success (`4c4378a1`). |
| `evilportal -duration` validation | `-duration -5` and `-duration abc` are both rejected and start no portal. Before `4c4378a1` both mapped to `0` = unlimited, disarming the only recovery path that survives `ble api off`. |
| `blespam apple` | iPhone showed "Setup New iPhone". Company ID 76 captured. |
| `blespam android` | 6 valid `0xFE2C` adverts captured. |
| `blespam fastpair_regular` (and `_fun`/`_prank`/`_custom`) | **Fixed in `c9c43c03`**, confirmed at handset level 2026-07-29 — Android Fast Pair popup, with 16 valid `0xFE2C` adverts captured concurrently across five model IDs. |
| `blespam samsung` / `windows` / `ibeacon` | Transmit correctly; name/UUID/BT-MAC now survive (`c9c43c03`). `samsung` drops its Flags AD structure (ISSUE-10, cosmetic). |
| Read/control verbs | `systeminfo`, `free`, `uptime`, `ls`, `cat`, `md5`, `crc32`, `storage stat/copy/rename/write`, `mkdir`, `rmdir`, `rm`, `i2c`, `loader list/open`, `webui -bg/-off`, `reboot` |
| `gpio` read/mode/set | Pins **47/48 only** on this board. Empty reply = success. |
| `settings` (read all) | Returns 1,917 B of JSON over BLE (`b1c825c8`). |
| `settings <field> <value>` | Confirms the write, or says the field is read-only (`b1c825c8`). |
| `encrypt` / `decrypt` | Round-trip 8/8 (`b1c825c8`). XOR+MD5 — obfuscation, not encryption. |
| `blespam samsung` | Flags no longer overflow; 0 errors per 30 packets (`b1c825c8`). |
| `screen brightness` | Takes **0–255**, not 0–100. |
| BLE transport | Framing, EOT, chunking, event characteristic, gap-free event IDs, boot persistence. |
| HTTP `POST /cm` + auth | Cookie and 401 paths verified. AP gateway is **172.0.0.1**. |
| **All HTTP routes** | `/systeminfo`, `/getscreen`, `/listfiles`, `/file`, `/`, `POST /cm` — all 200 and sub-second, **but only with `ble api off`**. See contract §1. |
| `wifi add` / `on` / `off` | Verified. `add` is a silent success; `on` falls back to AP in 9.3 s and costs ~53 KB. |
| Transport switch | BLE → `ble api off` → HTTP bulk → `POST /cm cmnd=ble api on` → BLE. Round trip verified; each transport restores the other. |
| `blespam random`/`all` | Works, recovers with factory MAC and name intact. |
| GATT liveness probe | Reading `0x2A19` works **while the CLI is blocked** — the only way to tell "blocked" from "dead". |
| HTTP `nav` rescue | Releases a blocked verb — **but see the limits in ISSUE-19.** |

## Broken — do not ship

| Capability | Failure | Entry |
|---|---|---|
| `deauth` | Crashes the device (SPI mutex, cross-task) | ISSUE-1 |
| `evilportal` (**blocking** form) | Crashes **under load** with BLE armed (ISSUE-1) — still draws from the serial task, unchanged by the headless work. Also commits DNS/HTTP state even when the AP failed to start (ISSUE-28). Use `-bg` instead |
| `badusb` (USB HID) | Types nothing. No longer hangs — returns in 9.3 s (`b1c825c8`) | ISSUE-20 |
| `badusb` (BLE HID) | **Not reachable** — no CLI path exists at all | ISSUE-20 |
| Serial CLI over USB | **Does not exist on this board.** BLE is the only command interface | ISSUE-22 |
| HTTP **bodies** with BLE armed | Small replies work; a real page body does not. TCP 80 accepts, `GET /` returns 0 bytes. **Corrected 2026-07-30** — this row previously read "both transports cannot coexist", which is wrong: BLE + AP + WebUI ran together and served `POST /login` and `POST /cm` fine | ISSUE-16, ISSUE-21 |
| `js` output/errors | Interpreter runs, but no return channel at all | ISSUE-15 |
| `rf rx`, `md5`/`stat` on a missing file | Silent failure, empty reply | ISSUE-13 §context |
| `rf selftest`, `nrf24`, `gps`, `getscreen` | Not registered in this build | — |

## Constraints the app must design around

| Constraint | Detail |
|---|---|
| No remote rescue for radio verbs | WiFi verbs kill the WebUI on entry; BLE verbs destroy WiFi. `deauth`, `karma`, `evilportal`, `sniffer`, `blesniffer` cannot be stopped remotely over **either** transport (ISSUE-19). |
| `nav` rescue needs repeated pulses | One pulse fails. Pulse until the device answers. Minimum not bisected. |
| WebUI margin is ~18 KB | Starts from a clean boot; fails silently if anything consumed heap first (ISSUE-12). A JS run alone is enough to break it (ISSUE-17). |
| Discover by service UUID, never by name | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91`. |
| Transports alternate, never coexist | Start the WebUI **before** dropping BLE, or the device is stranded (ISSUE-22). |
| `POST /login` is form-encoded | Fields are `username`/`password`, **not** JSON `{user,pwd}`. Wrong form fails silently with a 200 and no cookie. |
| Never store real WiFi credentials | Plaintext in the config, readable over an unauthenticated BLE link (ISSUE-23). |
| BLE replies can truncate silently | No `[TRUNCATED]` marker. Treat a missing EOT as "retry" (ISSUE-16). Hash files, don't eyeball listings. |
| No battery UI | `battery_pct` still permanently 1 (ISSUE-3 partial — the I²C storm is fixed, the reporting is not). |
| Never gate on `capabilities` | Compile-time flags (ISSUE-4). |
| `POST /login` writes flash every time | And can abort the device under load (ISSUE-18). |
| HTTP `/cm` leaves the screen stale | Fix landed (`d71f19e9`) but **unverified on hardware**, and it only repaints when the main loop is on the **main menu** — a submenu drops the request until a button press (ISSUE-24). |
| Start `webui` immediately after boot | The margin is decided by **under 1 KB**, and navigating menus first is enough to flip it. Three fresh boots, same command, two outcomes (ISSUE-12). |
| A "working" RAMLOG profile does not mean HTTP will serve | Two runs matched within 580 bytes and only one could complete a login. `dma largest` at `webui post-begin` predicts better than free heap — 6,644/6,900 served, 6,132 did not (SUSPECTED, ISSUE-12). |
| A failed GATT **write** means retry; an empty **reply** means do not | ISSUE-26 vs ISSUE-16 — opposite handling, distinguishable at the client by whether the write raised. |
| `BLE_INIT: Malloc failed` on the console | The device is one allocation from an `abort()` reboot (ISSUE-25). |
| The main loop can wedge while BLE still answers | Remote surfaces responding is **not** evidence the device is usable at the board. Frozen status-bar clock is the cheap liveness probe (ISSUE-30). |
| Evil Portal gateway is `172.0.0.1`, never `192.168.4.1` | The phone-friendly default is dead code on any configured device (ISSUE-27). |

---

## Untested, with the reason

The honest gap. See §"Not tested, and why" in KNOWN_ISSUES.md for the full table.

- **Every non-`deauth` blocking verb under load** — `karma`, `pwngrid`, `ap_info`,
  `blesniffer`. Now the highest-value unknown, because ISSUE-1 turned out to be about
  *any* sustained drawing from the serial task, not `drawArc` specifically.
- **`badusb` over BLE HID** — a different branch (`ducky_startKb(..., ble=true)`) that
  never touches TinyUSB, so ISSUE-20 may not apply. More useful than the USB path.

- **`/upload`, `/edit`, `/rename`, WS `/ws`** — the write-side HTTP routes and the
  WebSocket. The read-side routes are all verified; these were not exercised.
- ~~**Evil Portal with `ble api off`**~~ — **DONE 2026-07-29.** It was the memory
  ceiling: the whole flow works in the BLE-off configuration, credential capture
  included (ISSUE-21). Not re-run since the headless verb landed.
- **ISSUE-24's fix (`d71f19e9`) on hardware** — **still unverified after three
  attempts** on 2026-07-30. The only discriminating test is
  `POST /cm cmnd=evilportal` with the device on the **main menu**; a submenu cannot
  work, because `loopOptions()` only consumes `returnToMenu` under `MENU_TYPE_MAIN`.
- **Credential capture with the headless portal** — approved, never run. Needs
  `ble api off` plus a short duration cap.
- **ISSUE-30's root cause** — the main-loop wedge. No backtrace obtainable; next step
  is `display.cpp:744` `Serial.println` → `log_e`.
- **`blesniffer` / `karma` / `pwngrid` / `ap_info` genuinely under load** — `blesniffer`
  now has 13 min of continuous redraw behind it, but no external load was applied.
- **`poweroff` / `sleep`** — need someone present to power-cycle.
- **MTU 247** — BlueZ caps at 128; needs an Android client.
- **ISSUE-17 leak vs. one-off cache** — needs `free` sampled after each `js` run.

---

## Test harness

`tools/ble_spike/` — needs `pip install bleak` in a venv.

| Script | Purpose |
|---|---|
| `bcli.py` | Batch CLI over BLE. **Discovers by service UUID**, and flags a missing EOT rather than hiding it — this is what surfaced ISSUE-16. |
| `feedverb.py` | Drives verbs that consume stdin lines until `EOF` (`storage write`, `encrypt`, `js run_from_buffer`). |
| `portalwatch.py` | Dispatches a blocking verb and keeps the **event** characteristic subscribed throughout — the CLI goes silent but events keep arriving. |
| `issue9_verify.py` | Checks name/UUID/BT-MAC survival across a spam. |
| `cryptotest.py` | The ISSUE-13 falsifiable round-trip test. |
| `usbwatch2.py` | Captures `/dev/ttyACM0`. **The only place a panic backtrace appears.** |
| `sniff2.py` | Packet-captures what is actually transmitted. |
| `portal_bg.py` | Headless-portal bench. `--cap-only` is unattended and asserts the duration cap fires, watching for stray bytes on the CLI characteristic rather than trusting the reply. |
| `http_routes.py` | Sweeps the HTTP routes once a station is associated. |
| `verbtest.py`, `swaptest.py`, `waitready.py`, `heap_poll.py`, `probe_verbs.py` | Earlier harness. |

**Only ever run one `usbwatch2.py` at a time** — two instances silently split the
stream between them and both look empty.

Decode a backtrace with:

```sh
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/smoochiee-board/firmware.elf <addresses>
```

Check the panic's `ELF file SHA256` against the local ELF first, or the decode is
fiction.

**NetworkManager gotcha:** joining the device's AP fails with *"Wi-Fi network could not
be found"* when the real cause is ARP duplicate-address detection hanging — the ESP32
does not answer ARP probes. Fix with
`nmcli connection modify <profile> ipv4.dad-timeout 0`. The supplicant log will show
the 4-way handshake completing and a DHCP lease issued while nmcli reports failure.

The same failure also shows up as **"IP configuration could not be reserved"**
(observed 2026-07-30), which is the more honest message: association succeeded and only
addressing failed. A **static address** works around it —
`ipv4.method manual`, `172.0.0.5/24`, no gateway, `ipv4.never-default yes` — and that is
what unblocked the session. But **try `ipv4.dad-timeout 0` first**; it is the documented
fix, it was missed on the day, and a static address hides a DHCP problem rather than
solving it.
