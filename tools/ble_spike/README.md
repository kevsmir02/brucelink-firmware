# BLE control-link bench scripts

Hardware smoke tests for the BLE control link, driven from a Linux laptop over
BlueZ. **No phone and no app dev build required** — they exercise the same
transport semantics `BleTransport` will, so a firmware change can be validated
in ~30 seconds instead of an Android build cycle.

## Setup

```bash
python3 -m venv .venv && .venv/bin/pip install bleak
```

The device must be advertising as `Bruc`. Since `623d4d26` the BLE API state is
persisted (`bleApiAutoStart`) and re-armed at boot, so **Config → Toggle BLE API**
is only needed once — but a flash that wipes the config file resets it to off.

## Running

```bash
.venv/bin/python tools/ble_spike/spike_transport.py
.venv/bin/python tools/ble_spike/spike_events.py
```

## The 2026-07-29 investigation harness

Added while auditing the verb surface against hardware. These are general-purpose;
the `spike_*.py` scripts below are the original targeted regression tests.

| Script | Use |
|---|---|
| `blecli.py <verb>...` | Send arbitrary verbs over BLE, print each reply. The workhorse. |
| `waitready.py <sec>` | Poll until the device answers `uptime` again. Chains a test to start automatically after a manual RST. |
| `usbwatch2.py <sec>` | Capture `/dev/ttyACM0` with timestamps, reopening across reboots. **The only place a panic backtrace appears.** |
| `verbtest.py <verb> <sec>` | Dispatch a verb, capture BLE + USB together, print a VERDICT with any assertion/backtrace. Used for the crash sweep. |
| `swaptest.py <verb>` | Full `blespam` transport-swap trace: suspend notice, link drop, `RAM_LOG` stage markers, recovery timing. |
| `sniff2.py <verb> <sec>` | **Packet capture.** Sends a spam verb, waits for the BLE API to suspend itself, then scans and tallies what is actually on the air by company ID and service UUID. |

`sniff2.py` is the one to reach for when a feature "does nothing" — it settled
ISSUE-8 by showing the FastPair payload going out under a bogus company ID, with no
handset involved.

**Discovery caveat that cost a day.** Do not identify the device with
`find_device_by_name("Bruc")`. After several spam types the name and service UUID are
dropped from the advertisement and the BT MAC changes (ISSUE-9), so name-based
discovery reports a healthy device as missing. Match on the service UUID, or connect
by address and probe for the CLI characteristic.

## What each covers

**`spike_transport.py`** — the CLI characteristic (`d555ed97-…`):

- MTU negotiation and whether the firmware chunks to the negotiated size.
  Report the *chunk sizes*, not the client's reported MTU: bleak's `mtu_size` is
  an uninitialised placeholder that reads 23 regardless. Chunk size is `mtu - 3`.
- A command split across two writes reassembles correctly (regression for the
  fragmentation bug fixed in `a512e3c1`).
- Positional response correlation across back-to-back commands, which the design
  relies on because the CLI carries no request IDs.
- Per-command latency.

**`spike_events.py`** — the event characteristic (`d555ed98-…`) and framing:

- Responses terminate with `EOT` (`0x04`), which unlike the `# ` prompt cannot
  appear in CLI text output.
- Event JSON arrives on its own characteristic and never interleaves with CLI
  stdout.
- Event IDs are monotonic and gap-free.

## Reference values

| | |
|---|---|
| Advertised name | `Bruc` |
| Service | `4371ec0b-3d43-49f9-b731-7c72a4a7bb91` |
| CLI characteristic | `d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9` |
| Event characteristic | `d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9` |
| Response terminator | `0x04` (EOT) |

Measured 2026-07-27 on smoochiee-board: BlueZ negotiates MTU 128 (125-byte
chunks); nRF Connect on Android leaves it at the default 23 (20-byte chunks)
because it never requests more. The app must call `requestMTU(247)` on connect.

Full bench protocol and results: `maritest/docs/BLE_BENCH_RUNBOOK.md`.
