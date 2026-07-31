# How to test a change

Three levels, cheapest first. [TEST_STATUS.md](./TEST_STATUS.md) records what these have
*found*; this page is how to run them.

---

## Three rules that override everything

1. **An idle test proves nothing.** `evilportal` survived 100 s idle and then crashed under
   load with the same assertion as `deauth` ([ISSUE-1](./KNOWN_ISSUES.md)). Every
   "survivor" in the seven-verb crash sweep was tested idle for 90 s; their clean results
   establish nothing.
2. **A compile-time `#define` is not a capability.** `USB_as_HID` is defined and BadUSB
   still cannot work (ISSUE-20); `/systeminfo` reports seven devices that are not fitted
   (ISSUE-4); `rf selftest` is documented upstream but not compiled in.
3. **Silence is not success.** Several verbs return an empty reply on success (`gpio`),
   several return an empty reply on *failure* (`md5`, `stat`, `decrypt`, `settings`), and
   `[CLI] Result: TRUE` is hardcoded for the five interactive attack verbs (ISSUE-7).

---

## 1. Host-side unit tests

```sh
pio test -e native
```

**34 test cases, 34 passing** at HEAD `26210f95` (2026-08-01):

| Suite | Covers |
|---|---|
| `test/test_byte_ring/` | The BLE RX FIFO — wraparound, line framing, overflow |
| `test/test_portal_cap/` | Evil Portal duration/cap policy |
| `test/test_crash_report/` | Core-dump rendering, including corrupted and missing fields |
| `test/test_webui_gate/` | WebUI start-gate decisions and report formatting |

`[env:native]` builds pure-logic files only (`test_build_src = no`). Anything that touches
Arduino, FreeRTOS or hardware **cannot** be tested here.

**This is why new logic is extracted into headers.** `webui_gate.h`, `portal_cap.h`,
`ByteRing.h` and `crash_report.h` exist as separate pure units specifically so a decision
can be tested without a device. Prefer that shape for anything new.

---

## 2. Bench scripts — `tools/ble_spike/`

Hardware smoke tests driven from a Linux laptop over BlueZ. **No phone and no app build
required** — they exercise the same transport semantics the app does, so a firmware change
can be validated in ~30 seconds instead of an Android build cycle.

```sh
python3 -m venv .venv && .venv/bin/pip install bleak
.venv/bin/python tools/ble_spike/spike_transport.py
```

The device must be advertising as `Bruc`, which means the BLE API must be armed — one-time
**Config → Toggle BLE API** on the device, persisted since `623d4d26`.

| Script | Use |
|---|---|
| `blecli.py <verb>…` | Send arbitrary verbs, print each reply. The workhorse. |
| `spike_transport.py` | Connect, MTU, write, chunked notify, EOT framing |
| `spike_events.py` | Event characteristic — subscription, frame shape, ID continuity |
| `spike_swap.py` / `swaptest.py` | Full BLE→Wi-Fi→BLE transport swap, with recovery timing |
| `spike_suspend.py` | BLE API suspend/resume around a radio verb |
| `heap_poll.py` | Poll `free` over time — the cheap check for a leaked AP |
| `probe_verbs.py` | Partial verb sweep (see caveat below) |
| `verbtest.py <verb> <sec>` | Dispatch a verb, capture BLE **and** USB together, print a VERDICT with any assertion or backtrace. Used for the crash sweep. |
| `usbwatch2.py <sec>` | Capture the console with timestamps, reopening across reboots |
| `waitready.py <sec>` | Poll until the device answers `uptime` again — chains a test after a manual reset |
| `sniff2.py <verb> <sec>` | **Packet capture.** Sends a spam verb, waits for the BLE API to suspend, then tallies what is actually on the air by company ID and service UUID. |

`sniff2.py` is the one to reach for when a feature "does nothing" — it settled ISSUE-8 by
showing the FastPair payload going out under a bogus company ID, with no handset involved.

> **Two caveats that each cost real time.**
>
> **Do not discover the device by name.** After several `blespam` types the name and
> service UUID are dropped from the advertisement and the BT MAC changes (ISSUE-9), so
> `find_device_by_name("Bruc")` reports a healthy device as missing. Match on the service
> UUID, or connect by address and probe for the CLI characteristic.
>
> **`probe_verbs.py` is deliberately partial.** It excludes destructive, blocking and
> menu-opening verbs. That exclusion list should stay excluded from any blind sweep.

Serial-side helpers live in `tools/serial/`: `bootlog.py`, `listen.py`, `cli.py`.

---

## 3. On-device testing

### Crash-testing a verb

Dispatch it over BLE while capturing the console port (`/dev/ttyACM0`, 115200 raw). A panic
prints `assert failed` / `Backtrace:` there — and, since `0f4936d1`, also survives in the
stored core dump, so a missed capture is no longer fatal to the investigation.

`verbtest.py` does both halves at once. Decoding is covered in
[BUILDING.md §Decoding a crash](./BUILDING.md#decoding-a-crash).

### Testing under load, not idle

Rule 1 above is the whole point. The ISSUE-1 crashes come from **sustained drawing from
the serial task while the main loop draws the same display** — so a verb must be exercised
while the screen is being repainted, not left sitting on a menu. The main loop repaints its
status bar on a 30 s timer, which is the cheapest source of contention available.

### Getting a blocked verb back

A verb that opens a TFT menu holds the serial task for its entire life, and **cannot be
rescued over BLE** — only `POST /cm cmnd=nav esc` over HTTP. Two further traps:

- **The dismissal key differs per verb.** `ap_info` exits on **SELECT only** and ignores
  Esc; menu verbs take Esc or a "Main Menu" entry; `evilportal` takes Esc *then*
  "Exit Portal".
- **On a dimmed screen the first button press is swallowed.** `InputHandler()` returns
  early when `wakeUpScreen()` reports it woke the display
  (`boards/smoochiee-board/interface.cpp`), so any "press LEFT+RIGHT to exit" instruction
  needs **two** presses.
- **A verb that binds port 80 has no remote rescue at all.** `reverseshell` runs its own
  AsyncWebServer on 80, so the WebUI and its `/cm nav esc` are gone for the verb's whole
  life. An operator at the board is the only exit — budget for that before starting, not
  after.

### Checking for a leaked radio

Radio verbs that exit cleanly may still leave their AP on air — `EvilPortal` did
(ISSUE-31) and `reverseshell` still does (ISSUE-39). **`free` returning to its idle plateau
is the cheap check**: 18,387 versus 81,327 is the whole tell. An independent `nmcli dev
wifi list` confirms whether the SSID is genuinely off the air.

---

## Writing up a result

Results go into [TEST_STATUS.md](./TEST_STATUS.md) (coverage) and
[KNOWN_ISSUES.md](./KNOWN_ISSUES.md) (defects), following the conventions in
[the docs index](./README.md#how-these-documents-are-written): device and date for a
measurement, `file:line` for a code claim, VERIFIED versus SUSPECTED kept distinct, and
"no crash observed in N s" rather than "safe".
