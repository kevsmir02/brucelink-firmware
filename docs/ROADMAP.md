# Roadmap

Where this fork is going. **This is intent, not a schedule.** It is a personal project with
no release cadence, no support commitment and no promise that any given commit builds for
your board.

Written 2026-08-01, at HEAD `26210f95`.

## Where it stands today

The thing this fork was built for works: **[brucelink](https://github.com/kevsmir02/brucelink)**,
the companion app, is shipped and driving this firmware over BLE. The transport, the command
bus, the event stream and the headless Evil Portal are all verified on hardware — see
[TEST_STATUS.md](./TEST_STATUS.md) for the line-by-line coverage.

What is left is the three things below.

---

## 1. Fit the missing modules

**Why.** The reference unit is a bare devkit. CC1101, NRF24, PN532, GPS, IR — none of them
are fitted, so every RF, RFID, wardriving and IR verb in this firmware is **completely
untested**. Worse, `/systeminfo` currently reports all of them as present
([ISSUE-4](./KNOWN_ISSUES.md)), so the honest position is that a whole class of features is
both unverified *and* misadvertised.

**Done means:**

- The modules are physically on the board.
- Each verb in `rf_commands.cpp`, `rfid_commands.cpp` and `ir_commands.cpp` has been run
  against real hardware and lands in [TEST_STATUS.md](./TEST_STATUS.md) with a device and a
  date — including the ones that turn out not to work.
- `capabilities` reports what is *fitted*, not what the board profile `#define`s. That
  needs runtime probing, which does not exist yet.
- The RF debug verbs (`rf selftest`, `keeloqtest`) are either compiled in via `-DRF_DEBUG=1`
  or documented as absent — right now the docs and the binary disagree.

**Blocked on:** having the hardware. Nothing else.

---

## 2. Close the open stability issues

**Why.** Several attack verbs are not safe to expose as one-tap actions in an app, and the
app cannot tell the difference from the outside.

The ones that matter most:

| Issue | State |
|---|---|
| **ISSUE-1** — TFT/SPI mutex race crashes the device from any sustained drawing on the serial task | OPEN, **mitigated**. Re-scoping `tftMutex` to the SPI transaction took `deauth` from crashing 2/2 in 20–70 s to 21 min clean. Open because that is a probabilistic result against a race. |
| **ISSUE-25** — the BT controller aborts the device when internal DMA runs out | OPEN, critical |
| **ISSUE-30** — the main loop task can wedge while every other task keeps running | OPEN. Panics nothing, so writes no core dump — the hardest class to diagnose here. |
| **ISSUE-12** — `webui` starts with ~18 KB of margin and fails if anything consumed heap first | OPEN. RC3 made the *start* report a real outcome; a request-time allocation failure is still unhandled. |
| **ISSUE-43** — the WebUI gate's refusal path panicked in the Wi-Fi driver, 1 run in 3 | OPEN |
| **ISSUE-26** — the BLE GATT *write* is rejected under memory pressure, losing the command | OPEN |
| **ISSUE-16** — BLE replies truncate silently under fragmentation, with no marker | OPEN |
| **ISSUE-39** / **ISSUE-31** — radio verbs leaving their AP on air after a clean exit | Fixes shipped, **awaiting verification** |

Also outstanding: a crash seen during the app's hardware smoke test on 2026-07-31 — Evil
Portal's credential write panicking under heap exhaustion — which is **not yet written up
as a formal issue**. It needs a reproduction before it earns an entry.

**Done means:** the interactive attack verbs can be dispatched from the app under load
without taking the device down, and the ones that still can't are labelled that way in
[TEST_STATUS.md](./TEST_STATUS.md) rather than quietly shipped.

**Note on method:** every one of these was found by testing, not by reading. The
[three rules](./TESTING.md#three-rules-that-override-everything) — an idle test proves
nothing, a `#define` is not a capability, silence is not success — are what closing them
depends on.

---

## 3. Track upstream Bruce

**Why.** Upstream moves. This fork is 124 commits past `59e83bfb` and deliberately stays
mergeable: new functionality goes into new files (`attack_commands.cpp`, `ws_events.cpp`,
`system_info.cpp`, `evil_portal_bg.cpp`) rather than being scattered through upstream code,
so a rebase is a manageable operation instead of a rewrite.

**Done means:**

- Periodically rebasing onto newer upstream and re-running the bench suite, so "mergeable"
  stays a measured claim rather than an architectural aspiration.
- Tracking upstream's [2.0 line](../2.0_road_path.md) — the move to Arduino-ESP32 v3.x and
  ESP32-C5 support. This fork already builds on the pioarduino platform, but a C5 has a
  different memory profile, and the radio-coexistence work here is tuned to the S3's.
- Keeping the fork's modifications to upstream files minimal enough to list on one screen
  ([ARCHITECTURE.md §9](./ARCHITECTURE.md#9-what-is-new-versus-what-was-modified)).

**Not planned:** upstreaming any of this. The changes are shaped around one board and one
companion app.

---

## Explicitly not planned

Listed so nobody plans against them:

- **Support for other boards.** Every environment except `smoochiee-board` is untouched and
  unverified. Making a second board work is a real project, not a config change.
- **Server-side event replay.** `wsEventId` is gap-free so a client can *detect* what it
  missed; storing frames to resend them was judged not worth the RAM.
- **A remote abort path over BLE.** `POST /cm cmnd=nav esc` over HTTP is the only remote
  stop, and it does not exist at all for verbs that bind port 80.
- **Targeted `deauth`.** The upstream entry point takes no target; a non-empty argument is
  rejected rather than silently ignored (ISSUE-5).
