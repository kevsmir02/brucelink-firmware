#!/usr/bin/env python
"""Task 8 verification — BLE API suspend/resume around a BLE attack.

This is the crash-prone path. An earlier implementation was reverted in
e2631370 ("Attempted pause/resume caused a crash on re-init"), so a single
successful pass proves nothing — the plan calls for three cycles without a
power-cycle.

Crash detection is unambiguous here. The BLE API is NOT persisted across
reboots, so:

  device resumed correctly -> re-advertises as "Bruc"
  device crashed/rebooted  -> BLE API comes back OFF, "Bruc" never reappears

So "did Bruc come back?" is a reliable reboot detector, no serial console
needed — which matters because opening /dev/ttyACM0 resets this board and
would switch the BLE API off itself.

Usage:  spike_suspend.py [cycles]      (default 3)
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Bruc"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04
SPAM_CMD = "blespam fastpair_regular 3"
REDISCOVER_TIMEOUT = 45.0


async def find(timeout=20.0):
    return await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)


async def exec_cmd(client, buf, done, cmd, wait=10.0):
    buf.clear()
    done.clear()
    await client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
    try:
        await asyncio.wait_for(done.wait(), wait)
    except asyncio.TimeoutError:
        pass
    return buf.split(bytes([EOT]))[0].decode("utf-8", "replace")


async def one_cycle(n):
    print(f"\n{'=' * 58}\nCYCLE {n}\n{'=' * 58}")

    dev = await find()
    if not dev:
        print(f"  FAIL: {DEVICE_NAME!r} not advertising at cycle start")
        return False
    print(f"  found {dev.address}")

    buf = bytearray()
    done = asyncio.Event()

    def on_cli(_s, data: bytearray):
        buf.extend(data)
        if EOT in buf:
            done.set()

    async with BleakClient(dev, timeout=20.0) as client:
        await client.start_notify(CLI, on_cli)

        pre = await exec_cmd(client, buf, done, "info")
        if "Bruce" not in pre:
            print(f"  FAIL: control link not working before the attack: {pre[:120]!r}")
            return False
        print("  pre-attack  : info responds")

        # The reply to this arrives, then the link drops as the GATT server is
        # torn down — so no EOT. A short wait is expected to time out.
        warned = await exec_cmd(client, buf, done, SPAM_CMD, wait=6.0)
        if "suspended" in warned:
            print(f"  suspend     : warned over BLE before dropping — {warned.strip()[:80]!r}")
        else:
            print(f"  suspend     : no warning seen (got {warned.strip()[:80]!r})")

    print(f"  attack      : running {SPAM_CMD!r}, waiting for the device to come back…")

    t0 = time.perf_counter()
    dev = await find(timeout=REDISCOVER_TIMEOUT)
    elapsed = time.perf_counter() - t0
    if not dev:
        print(f"  FAIL: {DEVICE_NAME!r} never came back within {REDISCOVER_TIMEOUT:.0f}s.")
        print("        The BLE API is not persisted across reboots, so this")
        print("        strongly suggests the device CRASHED and rebooted.")
        return False
    print(f"  resume      : re-advertising after {elapsed:.1f}s")

    buf2 = bytearray()
    done2 = asyncio.Event()

    def on_cli2(_s, data: bytearray):
        buf2.extend(data)
        if EOT in buf2:
            done2.set()

    async with BleakClient(dev, timeout=20.0) as client:
        await client.start_notify(CLI, on_cli2)
        post = await exec_cmd(client, buf2, done2, "info")

    if "Bruce" not in post:
        print(f"  FAIL: control link dead after resume: {post[:120]!r}")
        return False
    print("  post-attack : info responds — control link rebuilt")
    return True


async def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    print(f"Task 8 suspend/resume verification — {cycles} cycles, no power-cycle between them.")
    print("Device must have the BLE API ON (Config -> Toggle BLE API).")

    results = []
    for i in range(1, cycles + 1):
        ok = await one_cycle(i)
        results.append(ok)
        if not ok:
            print(f"\nStopped at cycle {i}. Do not iterate blindly on a crash in this path —")
            print("capture the serial backtrace with no BLE client attached instead.")
            break
        if i < cycles:
            await asyncio.sleep(2.0)

    print(f"\n{'=' * 58}")
    passed = sum(results)
    print(f"RESULT: {passed}/{cycles} cycles passed")
    if passed == cycles:
        print("The rebuild is stable across repeated suspend/resume pairs.")
    return 0 if passed == cycles else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
