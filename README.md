# Bruce — Companion Fork

> A personal, unofficial fork of [Bruce](https://github.com/pr3y/Bruce). **Not** the
> official firmware, **not** supported by the Bruce team. For the real thing, go to
> [bruce.computer](https://bruce.computer/flasher).

ESP32 firmware modified so a **mobile app can drive the device remotely**. Upstream Bruce
is operated from its own screen and buttons; this fork exposes the same capabilities over a
command bus reachable from a phone, without removing anything.

The app is **[brucelink](https://github.com/kevsmir02/brucelink)** — shipped, and working
against this firmware.

## What the fork adds

Every Wi-Fi attack in Bruce tears down the Wi-Fi stack, so controlling the device over the
existing Web UI destroys the connection the moment you use it. **BLE became the primary
control transport**, with Wi-Fi kept for bulk transfer. BLE and Wi-Fi then compete for the
same scarce internal DMA memory, so most of the work is about never running both radios at
once and swapping cleanly between them.

Concretely: a BLE GATT command/event transport, a unified `/ws` event stream, remote-callable
CLI verbs for the attacks, a headless Evil Portal, an extended `/systeminfo`, a `crashlog`
verb that reads the stored core dump over the air — and a pile of crash fixes found along
the way.

## Tested hardware

**One board: Smoochiee V2 carrier with an ESP32-S3-N16R8** — 16 MB flash, 8 MB OPI PSRAM,
1.47" IPS LCD, five buttons. PlatformIO env `smoochiee-board`. That is the only hardware
this has ever run on.

The test unit is a **bare** one — no CC1101, NRF24, PN532, GPS, IR, PMU or SD card fitted —
so every RF, RFID and IR verb here is untested, and `/systeminfo` reports those modules as
present when they are not. See [docs/HARDWARE.md](./docs/HARDWARE.md).

**No claim is made about any other board.** The radio-coexistence work is tuned to this
chip's memory profile; boards with less internal DRAM may not work at all.

## Quick start

```sh
pio run -e smoochiee-board                 # build
pio run -e smoochiee-board -t upload       # flash
pio test -e native                         # host-side unit tests (34 cases)
```

Then enable the command interface once on the device: **Config → Toggle BLE API**. It
persists across reboots. Full instructions in [docs/BUILDING.md](./docs/BUILDING.md).

## Documentation

| | |
|---|---|
| [docs/](./docs/README.md) | Index and reading order |
| [ARCHITECTURE.md](./docs/ARCHITECTURE.md) | How remote control works |
| [HARDWARE.md](./docs/HARDWARE.md) | What the test unit actually is |
| [BUILDING.md](./docs/BUILDING.md) | Build, flash, decode a crash |
| [TESTING.md](./docs/TESTING.md) | How to verify a change |
| [TEST_STATUS.md](./docs/TEST_STATUS.md) | What has actually been proven on hardware |
| [KNOWN_ISSUES.md](./docs/KNOWN_ISSUES.md) | Verified defect register |
| [bruce-companion-api.md](./docs/bruce-companion-api.md) | The app↔firmware contract |
| [FIRMWARE_CHANGES.md](./docs/FIRMWARE_CHANGES.md) | What changed and why |

Documentation here is written to one rule: **evidence, not assertion**. Every claim is
either a code fact with a `file:line`, or a measurement with a device and a date.

## Roadmap

Intent, not a schedule — see [docs/ROADMAP.md](./docs/ROADMAP.md):

1. **Fit the missing modules** and actually verify the RF, RFID and IR verbs.
2. **Close the open stability issues** so the attack verbs are safe as one-tap app actions.
3. **Track upstream Bruce**, keeping the fork mergeable.

## Disclaimer

**Published for educational and research purposes only.** This is a personal learning
project: the point is understanding how ESP32 radios, BLE/Wi-Fi memory contention and
embedded transports actually behave, and building a companion app against that. It is
intended solely for **legal, authorized security testing** — on hardware and networks you
own, or have explicit written permission to test.

Deauthentication, rogue access points, credential capture and BLE advertisement spam can be
criminal offences and can disrupt systems other people depend on. **Do not use this on
anything that is not yours.**

By building, flashing or using this fork you accept that you are solely responsible for
complying with all applicable laws and radio regulations; that it is provided **as-is**,
with **no warranty**, no support and no guarantee it works on your hardware at all; and
that the author accepts **no liability** for any damage, data loss, bricked hardware or
legal consequence arising from its use — see the [AGPL-3.0 license](./LICENSE), §§15–17,
which govern. No affiliation with or endorsement by the upstream Bruce project is claimed.
Do not report problems with this fork to them.

## Upstream

Bruce is a versatile ESP32 firmware for red-team operations, supporting a long list of
M5Stack, Lilygo and other devices — none of which this fork has been built for. For its
feature list, supported hardware and documentation:

- Project: [github.com/pr3y/Bruce](https://github.com/pr3y/Bruce) · [bruce.computer](https://bruce.computer)
- Wiki: [wiki.bruce.computer](https://wiki.bruce.computer/)
- Third-party attribution and copyleft compliance: [THIRD_PARTY.md](./THIRD_PARTY.md)

Licensed under AGPL-3.0, as upstream is.
