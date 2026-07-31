# Building, flashing and debugging

Verified 2026-08-01 at HEAD `26210f95` on Fedora Linux, PlatformIO Core **6.1.19**,
Python **3.14.6**.

---

## Prerequisites

Only PlatformIO. Everything else — the Xtensa toolchain, the ESP32 platform, the Arduino
framework — is pinned in `platformio.ini` and downloaded on the first build.

```sh
pip install --user platformio      # or: pipx install platformio
```

Two things are pinned deliberately and should not be "upgraded" casually
(`platformio.ini` `[env]`):

| Pin | Version |
|---|---|
| `platform` | pioarduino `platform-espressif32` **55.03.39** (Arduino 3.3.9) |
| `platform_packages` | `framework-arduinoespressif32-libs` build **20260715-140131** (Arduino 3.3.10, exFAT support) |

The framework build's embedded timestamp is what makes this repo's builds reproducible —
see [Reproducible builds](#reproducible-builds).

---

## Build

```sh
pio run -e smoochiee-board                 # build
pio run -e smoochiee-board -t upload       # build + flash over USB
pio device monitor                         # 115200, exception decoder on
pio test -e native                         # host-side unit tests
```

`smoochiee-board` is **the only environment this fork has been built or run in**. The
upstream envs for M5Stack, Lilygo and the rest are still in the tree and untouched, but
nothing here has been compiled for them.

Measured at HEAD, warm `.pio/` (nothing recompiled, link cached):

```
RAM:   [====      ]  39.6% (used 129736 bytes from 327680 bytes)
Flash: [==        ]  23.0% (used 3857522 bytes from 16777216 bytes)
```

A first build from a cold cache downloads several hundred MB of toolchain and framework and
takes considerably longer.

### Build flags worth knowing

| Flag | Where | Effect |
|---|---|---|
| `-DCORE_DEBUG_LEVEL=1` | `boards/smoochiee-board/smoochiee-board.ini` | **`log_e` is the only compiled log level.** `log_i`/`log_w` are stripped — a diagnostic written with them does not exist in the binary. |
| `-D ENABLE_RAM_LOGGING=1` | `platformio.ini` `[env]` | `RAM_LOG()` stage markers, mirrored to UART0 TX (GPIO 43). |
| `-D ENABLE_RAM_SAMPLER=1` | not set | Opt-in periodic heap sampler. Its 4 KB task stack comes out of internal DRAM — larger than the margin it measures. |
| `-DRF_DEBUG=1` | not set | Would compile in `rf selftest`, `keeloqtest`, `keeloqfiletest` (`src/modules/rf/protocols/rf_config.h:19-21`). **They do not exist in this build**; the device answers `ERROR: Command not found`. |
| `-Os`, `-flto` | both | Size-optimised. Line numbers in backtraces are still accurate; inlining is not. |

---

## Flashing

`pio run -t upload` is the normal path. To flash the built artifacts by hand:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash \
  0x0     .pio/build/smoochiee-board/bootloader.bin \
  0x8000  .pio/build/smoochiee-board/partitions.bin \
  0x10000 .pio/build/smoochiee-board/firmware.bin
```

`firmware.factory.bin` in the same directory is the single merged image, flashable at
`0x0`.

> **A flash that wipes the config file resets `bleApiAutoStart` to off**, and the BLE
> command interface does not come back on its own. Re-enable it once on the device:
> **Config → Toggle BLE API**. There is no serial CLI on this board to do it remotely
> ([ISSUE-22](./KNOWN_ISSUES.md)).

### Partition layout — `custom_16Mb.csv`

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data | `0x9000` | 24 KB |
| `factory` | app | `0x10000` | 4.4 MB |
| `spiffs` | data (LittleFS) | `0x480000` | 11.4 MB |
| `coredump` | data | `0xFF0000` | 64 KB |

The `coredump` partition has been there since the fork's first commit; nothing read it
until `0f4936d1` added the `crashlog` verb.

---

## Serial ports

The reference unit exposes **two** USB devices, and they do different jobs. Getting this
wrong wastes hours — see [HARDWARE.md §USB ports](./HARDWARE.md#the-two-usb-ports).

| Port | What it carries |
|---|---|
| CH343 UART bridge (`1a86:55d3`, typically `/dev/ttyACM0`) | `ESP_LOG` output, panic backtraces, `RAM_LOG`. **The console.** |
| Native USB-CDC (`303a:1001`, typically `/dev/ttyACM1`) | Arduino `Serial`. Carries no CLI while the BLE API is armed. |

Helpers in `tools/serial/`: `bootlog.py` (capture a boot), `listen.py` (tail the console),
`cli.py` (send a verb over USB CDC — only useful with the BLE API disarmed).

---

## Reproducible builds

**The build is bit-for-bit reproducible per source revision.** Measured 2026-07-30: nine
commits were rebuilt and every one reproduced the ELF hash recorded for it at the time.
`esp_app_desc`'s embedded timestamp is the *framework's* build date, not the local compile
time, so nothing drifts between builds of the same source.

This is what makes a stored core dump decodable days later: check out its commit, rebuild,
`addr2line` against that ELF.

```sh
sha256sum .pio/build/smoochiee-board/firmware.elf | cut -c1-9
```

That one digest appears in three places: the panic handler's `ELF file SHA256`, and
`crashlog`'s `elf=` and `running_elf=` fields.

| Commit | ELF sha256 (first 16) |
|---|---|
| `26210f95` (HEAD, 2026-08-01) | `6b71b7b3ee47ea3a` |
| `253860a0` (RC3) | `8a30e3153a15326a` |
| `3629afd7` | `3dd17e72827f4325` |
| `afa17b57` | `2efaeec784a5768a` |
| `f54bbc6c` | `46d975be7d38f128` |
| `6fd2b5fc` | `e81b0c28f80e70dd` |
| `cedad77f`, `038c00fd` | `f5244eb35dd10795` (identical — the later is docs-only) |
| `22ab5974` | `411d7e151dbc2356` |
| `4c4378a1` | `76d42c72f2b4a8a4` |
| `881ade6f` | `b919402708ab4de0` |
| `2d9422ea` | `5186685c0fdf19c2` |

⚠️ **Reproducible per *source*, not per *behaviour*.** `log_e` bakes `__LINE__` into the
binary, so inserting a comment above one shifts the hash without changing what the firmware
does — adding one comment block took `2b39286a` from `79fc138cd` to `fabcc0003`. A
`match=NO` from `crashlog` therefore means "not this exact source", which is stricter than
"not this code", and that is the behaviour you want from a guard.

⚠️ **`TEST_STATUS.md` once credited `76d42c72f2b4a8a4` to firmware `fbfe6226`. That was
wrong** — it is `4c4378a1`, confirmed by rebuild. Treat any commit-to-ELF attribution not
in the table above as unverified.

---

## Decoding a crash

A panic prints `assert failed` / `Backtrace:` on the **console port** and, since
`0f4936d1`, also writes an ELF core dump to flash that survives the reboot.

**Over the wire**, with nothing attached:

```
crashlog            # faulting task, exc_pc, backtrace, elf=, running_elf=, match=
crashlog -clear     # erase, so the next crash is unambiguous
```

**From a captured backtrace:**

```sh
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/smoochiee-board/firmware.elf <addresses>
```

Always check the panic's `ELF file SHA256` against the local ELF before trusting a decode.

Caveats:

- **A wedge panics nothing and writes no dump.** The main-loop task can stop while every
  other task keeps running ([ISSUE-30](./KNOWN_ISSUES.md)); core dumps do not help there.
- `CAPTURE_DRAM` is off — stacks and registers only.
- The partition is overwritten by the next crash. Read it before provoking another.
