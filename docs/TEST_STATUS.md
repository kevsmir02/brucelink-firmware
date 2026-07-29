# Test status — what is actually known to work

Coverage map for the fork, as of **2026-07-29**. Companion to
[KNOWN_ISSUES.md](./KNOWN_ISSUES.md) (the defect detail and evidence),
[bruce-companion-api.md](./bruce-companion-api.md) (the interface contract) and
[FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) (why the fork exists).

**Why this file exists.** The other three answer "what is it" and "what is broken".
This one answers "what has anyone actually run on hardware", so the next person can
tell a verified capability from an assumed one at a glance. Every row is either a
measurement (device + date) or a code fact (`file:line`). Nothing here is inferred.

**Hardware under test:** bare ESP32‑S3‑N16R8 devkit, 1.47" 172×320 IPS LCD on SPI,
five buttons, USB powered. No PMU, SD, CC1101, NRF24, PN532, IR or GPS. Env
`smoochiee-board`. Firmware `b1c825c8`, ELF `4bdcd1dc364fd2cf`.

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
| HTTP `nav` rescue | Releases a blocked verb — **but see the limits in ISSUE-19.** |

## Broken — do not ship

| Capability | Failure | Entry |
|---|---|---|
| `deauth` | Crashes the device (SPI mutex, cross-task) | ISSUE-1 |
| `evilportal` | Crashes **under load**, same assertion; and cannot serve its own page | ISSUE-1, ISSUE-21 |
| `badusb` (USB HID) | Types nothing. No longer hangs — returns in 9.3 s (`b1c825c8`) | ISSUE-20 |
| `badusb` (BLE HID) | **Untested** — different branch, may work | ISSUE-20 |
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
| BLE replies can truncate silently | No `[TRUNCATED]` marker. Treat a missing EOT as "retry" (ISSUE-16). Hash files, don't eyeball listings. |
| No battery UI | `battery_pct` still permanently 1 (ISSUE-3 partial — the I²C storm is fixed, the reporting is not). |
| Never gate on `capabilities` | Compile-time flags (ISSUE-4). |
| `POST /login` writes flash every time | And can abort the device under load (ISSUE-18). |

---

## Untested, with the reason

The honest gap. See §"Not tested, and why" in KNOWN_ISSUES.md for the full table.

- **Every non-`deauth` blocking verb under load** — `karma`, `pwngrid`, `ap_info`,
  `blesniffer`. Now the highest-value unknown, because ISSUE-1 turned out to be about
  *any* sustained drawing from the serial task, not `drawArc` specifically.
- **`badusb` over BLE HID** — a different branch (`ducky_startKb(..., ble=true)`) that
  never touches TinyUSB, so ISSUE-20 may not apply. More useful than the USB path.
- **Evil Portal credential capture** — blocked by ISSUE-21; there is no form to submit.
- **`wifi add` / `wifi on` / `wifi off`** — zero evidence. BLE WiFi provisioning is a
  headline app feature.
- **HTTP file routes** — `/getscreen`, `/listfiles`, `/file`, `/upload`, `/edit`,
  `/rename`, WS `/ws`.
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
