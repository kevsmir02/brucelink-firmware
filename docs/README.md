# Documentation index

This fork of [Bruce](https://github.com/pr3y/Bruce) makes an ESP32-S3 drivable from a
phone. These documents describe what it does, how it does it, what has actually been
proven on hardware, and what is known to be broken.

**Last full pass:** 2026-08-01, at HEAD `26210f95`.

## Start here

| Doc | Answers |
|---|---|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | How remote control works — the memory constraint, the command bus, the two transports, the event stream |
| [HARDWARE.md](./HARDWARE.md) | What the reference unit physically is, and where the board profile lies about it |
| [BUILDING.md](./BUILDING.md) | Toolchain, build, flash, partition layout, reproducible builds, decoding a crash |
| [TESTING.md](./TESTING.md) | How to test a change: host unit tests, the BLE bench harness, on-device crash capture |

## Reference

| Doc | Answers |
|---|---|
| [TEST_STATUS.md](./TEST_STATUS.md) | **What has actually been run on hardware** — shippable, broken, untested-and-why. Read this before promising a feature. |
| [KNOWN_ISSUES.md](./KNOWN_ISSUES.md) | Verified defect register, VERIFIED vs SUSPECTED tracked explicitly. Read before planning against any verb. |
| [bruce-companion-api.md](./bruce-companion-api.md) | The app↔firmware contract — UUIDs, framing, endpoints, event frames, per-verb cost |
| [FIRMWARE_CHANGES.md](./FIRMWARE_CHANGES.md) | Rationale — what changed by area with commit refs, and the approaches that failed |
| [ROADMAP.md](./ROADMAP.md) | Where this is going, and what "done" means for each item |

The consumer of this contract is the companion app,
**[brucelink](https://github.com/kevsmir02/brucelink)** — shipped and working against this
firmware. It vendors a copy of the API contract; changing the interface here makes that
copy stale.

## How these documents are written

The standing rule in this repo is **evidence, not assertion**:

- Every claim is either a **code fact** with a `file:line` citation, or a
  **measurement** with a device and a date. Anything else is marked **UNVERIFIED**.
- **VERIFIED and SUSPECTED are distinguished.** Generalising from one tested case to an
  untested one that merely looks similar is called out as such rather than stated flat.
- **A clean test window is not proof of safety.** `deauth` ran 70+ s before crashing on
  its first run, so results are written as "no crash observed in N s", never "safe".
- **Conditions travel with a measurement.** "Largest DMA block 1,332 bytes" means nothing
  without "fully loaded, with a station associated" — the same board reads 6,900 idle.
- **Line numbers drift.** Every citation was correct when written; grep for the symbol if
  a line does not match.
- Corrections are made **in place, with the correction visible** — superseded claims are
  struck through and dated rather than deleted, because knowing a doc was once wrong is
  itself useful.

`BRUCELINK.md` at the repo root is the condensed version of all of this, written for
coding agents. If it disagrees with a document here, the more recently verified one wins —
check the dates.
