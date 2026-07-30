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
rows are from firmware `fbfe6226`, ELF `76d42c72f2b4a8a4`; the 2026-07-30 fix rows
(ISSUE-5, 7, 15, 18, 23, 28, 29, 30, 31) are from ELF **`411d7e151dbc2356`**.

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
| **Evil Portal headless — `evilportal -bg`** | The verb no longer holds the serial task: `uptime` over BLE answered in **0.06 s** during a live portal, `-status` answered 9× during one. Duration cap self-stopped at **+45.6 s** on a 45 s cap, with **zero** stray bytes on the CLI characteristic. AP confirmed on air by an independent `nmcli` scan. ELF `76d42c72f2b4a8a4`, 2026-07-30. **Serves the full page with the BLE API armed as of ELF `e81b0c28f80e70dd`** — 8/8 loads returned all 4,726 bytes, byte-identical, 0.134–0.376 s, form included. The old "needs `ble api off`" restriction is **lifted** (ISSUE-21 resolved). |
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
| `evilportal` (blocking) — **leaks its AP** | Exiting without completing "Exit Portal" leaves the AP, DNS and web server **running and serving** (200 in 11 ms). Hardware-confirmed 2026-07-30. Destructor fix landed in `411d7e151dbc2356` but is **unverified** | ISSUE-31 |
| `reverseshell` — **leaks its AP on exit** | The verb now works end to end and exits cleanly (`cedad77f` closed the heap-corruption reboot, ISSUE-38). But the exit path never brings the radio down, so `BruceShell` keeps broadcasting WPA2 after the operator ends the attack, holding ~63 KB. Two scans plus a successful association confirmed it 2026-07-30 | ISSUE-39 |
| HTTP **bodies** with BLE armed | **Corrected twice.** This row first read "both transports cannot coexist" (wrong: BLE + AP + WebUI ran together and served `POST /login` and `POST /cm` fine), then "a real page body does not work" (also wrong). The portal's 4,726-byte page now serves 8/8 with BLE armed on ELF `e81b0c28f80e70dd`. What failed was one oversized `write()` losing the fourth TCP segment, not page bodies as a class — see ISSUE-21 §Root cause | ISSUE-16, ISSUE-21 |
| `js` output/errors | Interpreter runs, but no return channel at all | ISSUE-15 |
| `rf rx`, `md5`/`stat` on a missing file | Silent failure, empty reply | ISSUE-13 §context |
| `rf selftest`, `nrf24`, `gps`, `getscreen` | Not registered in this build | — |

## Constraints the app must design around

| Constraint | Detail |
|---|---|
| No remote rescue for radio verbs | WiFi verbs kill the WebUI on entry; BLE verbs destroy WiFi. `deauth`, `karma`, `evilportal`, `sniffer`, `blesniffer` cannot be stopped remotely over **either** transport (ISSUE-19). |
| `nav` rescue needs repeated pulses | One pulse fails. Pulse until the device answers. Minimum not bisected. |
| WebUI margin is ~18 KB | Starts from a clean boot; fails silently if anything consumed heap first (ISSUE-12). A JS run within the last ~2 s is enough to break it (ISSUE-17). |
| Discover by service UUID, never by name | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91`. |
| Transports alternate, never coexist | Start the WebUI **before** dropping BLE, or the device is stranded (ISSUE-22). |
| `POST /login` is form-encoded | Fields are `username`/`password`, **not** JSON `{user,pwd}`. Wrong form fails silently with a 200 and no cookie. |
| Never store real WiFi credentials | **Still plaintext in `bruce.conf`**, link still unauthenticated. The `settings` dump is now redacted (ISSUE-23), which closes casual disclosure but not file reads. |
| BLE replies can truncate silently | No `[TRUNCATED]` marker. Treat a missing EOT as "retry" (ISSUE-16). Hash files, don't eyeball listings. |
| No battery UI | `battery_pct` still permanently 1 (ISSUE-3 partial — the I²C storm is fixed, the reporting is not). |
| Never gate on `capabilities` | Compile-time flags (ISSUE-4). |
| `POST /login` no longer writes flash | Fixed 2026-07-30: sessions are RAM-only, logins are **67–237 ms** (was 350 ms–2.35 s), and 8 consecutive logins caused **no abort** where ~4 used to. Still **non-deterministic** under memory pressure — 3 of those 8 stalled (ISSUE-18, ISSUE-16). |
| HTTP `/cm` repaints the screen | **Verified on hardware 2026-07-30** (`d71f19e9`). Only when the main loop is on the **main menu** — a submenu still drops the request until a button press (ISSUE-24). |
| Don't navigate menus before `webui` | The margin is decided by **under 1 KB**. **Elapsed uptime is not the variable** — `webui -bg` at 12 min uptime with no navigation gave the best profile recorded (dma 6,900). Navigating first is what flips it (ISSUE-12). |
| Give a `js` script ~2 s before `webui` | The interpreter task's 16 KB stack is reclaimed by the FreeRTOS **idle task**, not at `vTaskDelete`. Start the WebUI inside that window and AsyncTCP cannot allocate; wait, and it is fine (ISSUE-17). |
| Two different HTTP failure modes | Connect **refused** + instant `http=000` = the server never started. Connect **accepted** + stall to timeout = the body cannot be allocated. Identical in curl, opposite meanings (ISSUE-17 vs ISSUE-16). |
| `js` output arrives on the **event** stream | `[js] …` `log` frames, asynchronously, *after* the verb's EOT — never in the verb's own reply (ISSUE-15). Script **errors** are still invisible. |
| Attack verbs emit `attack_result` | New frame type with `verb`/`outcome`/`elapsed_ms`/`wifi_mode`/`free_heap`. `outcome` is `completed`, **never** `success` — these are interactive menus (ISSUE-7). |
| A "working" RAMLOG profile does not mean HTTP will serve | Two runs matched within 580 bytes and only one could complete a login. `dma largest` at `webui post-begin` predicts better than free heap — 6,644/6,900 served, 6,132 did not (SUSPECTED, ISSUE-12). |
| A failed GATT **write** means retry; an empty **reply** means do not | ISSUE-26 vs ISSUE-16 — opposite handling, distinguishable at the client by whether the write raised. |
| `BLE_INIT: Malloc failed` on the console | The device is one allocation from an `abort()` reboot (ISSUE-25). |
| The main loop can wedge while BLE still answers | Remote surfaces responding is **not** evidence the device is usable at the board. Frozen status-bar clock is the cheap liveness probe (ISSUE-30). |
| Evil Portal gateway is `172.0.0.1`, never `192.168.4.1` | And that is fine: two real handsets auto-detected the portal on `172.0.0.1`. The `192.168.4.1` default was dead code and was **removed** in `cedad77f` (ISSUE-27, resolved). |
| `evilportal -status` reports AP health, not just liveness | `portal: running ap:up services:up ssid:… ` — reads `degraded` when the AP or its services are down, and the SSID is the live one after a `/ssid` rename (ISSUE-33, resolved in `cedad77f`). |
| `/ssid?ssid=` cannot kill a running portal | Empty and >32-byte values return **400** and leave the AP untouched; 32 bytes is accepted (ISSUE-34, resolved in `cedad77f`). |
| `reverseshell` exits without crashing | The heap-corruption reboot on its only exit path is fixed; uptime is continuous across the exit (ISSUE-38, resolved in `cedad77f`). **But it leaves its AP on air** — ISSUE-39. |

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
- ~~**Evil Portal with `ble api off`**~~ — **DONE 2026-07-29.** The whole flow works in
  the BLE-off configuration, credential capture included. The "memory ceiling" reading
  was wrong; the page now serves with BLE **armed** too (ISSUE-21 resolved 2026-07-30).
  Credential capture has **not** been re-run since the headless verb landed.
- ~~**ISSUE-24's fix (`d71f19e9`) on hardware**~~ — **DONE 2026-07-30, 5th attempt.**
  Verified with `POST /cm cmnd=blespam apple 10`, not `evilportal`: the portal verb is
  structurally unable to test this, because exiting it requires the very button
  presses that repaint the screen anyway. See ISSUE-24.
- **ISSUE-31's destructor fix** — the *defect* is now hardware-confirmed (a portal
  still serving 200 after exit), but the fix is not. Needs an attended run: start the
  blocking `evilportal`, leave without completing "Exit Portal", then re-probe the AP
  and `GET /hotspot-detect.html`.
- **ISSUE-28's failed-`softAP()` path** — code-verified only; `WiFi.softAP()` has not
  been made to fail on the bench.
- **ISSUE-29's fix, and its predicted effect on ISSUE-19** — clearing the latched
  button globals should also supply the release edge that `ScrollableTextArea` waits
  for, which may be why one `nav` pulse never sufficed. Both halves unverified; needs
  a blocked `ap_info` and a single pulse.
- ~~**`reverseshell` with a working AP**~~ — **DONE 2026-07-30, attended.** AP up (WPA2,
  ch 1), relay proven both directions, and the Esc-chord exit now returns without the
  heap-corruption reboot (ISSUE-38 resolved). What it exposed instead: the exit leaves
  the AP on air (ISSUE-39).
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

**NetworkManager gotcha — joining the device's AP.** It fails with *"Wi-Fi network
could not be found"* or *"IP configuration could not be reserved"*. **Use a static
address; this is the fix, not a workaround:**

```sh
nmcli con add type wifi ifname '*' con-name BruceNet ssid BruceNet -- \
  wifi-sec.key-mgmt wpa-psk wifi-sec.psk brucenet \
  ipv4.method manual ipv4.addresses 172.0.0.5/24 ipv4.gateway "" \
  ipv4.never-default yes ipv6.method disabled connection.autoconnect no
```

⚠️ **This note previously recommended `ipv4.dad-timeout 0` and blamed ARP duplicate-
address detection. That is WRONG and was disproven on 2026-07-30.** A fresh profile
with `dad-timeout 0` failed identically, and the NetworkManager journal shows why:

```
supplicant interface state: 4way_handshake -> completed
Activation: (wifi) Stage 2 of 5 (Device Configure) successful.
dhcp4 (wlp39s0): activation: beginning transaction (timeout in 45 seconds)   ... no lease
```

**Association succeeds; the device's DHCP server does not answer.** DAD is not
involved, so disabling it cannot help. The static address is not hiding a DHCP
problem — the DHCP problem is device-side, real, and unfixed. Verified with the
static profile: ICMP 3/3 to `172.0.0.1`, `POST /login` 302 + cookie in 0.35 s.

**The AP credentials are `BruceNet` / `brucenet`** (`settings wifiAp`), and the
gateway is **172.0.0.1**.

**The repo ships no `.venv`.** Create it before using any bench script:
`python3 -m venv .venv && .venv/bin/pip install bleak`.

⚠️ **Never `pkill -f usbwatch2`** — the pattern matches the invoking shell's own
command line and kills the whole session. Use `pgrep -af "python.*usb"` to find it.
