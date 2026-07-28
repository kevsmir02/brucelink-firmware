#!/usr/bin/env python
"""Verify the control-transport swap around a BLE attack (Task 8).

The claim under test is NOT "the BLE link survives a BLE attack" — it cannot,
and that was the wrong question. The claim is that control moves to the radio
the attack is not using:

  WiFi attack -> control on BLE   (already proven)
  BLE attack  -> control on WiFi  (this script)

So the decisive assertion is that THE AP IS STILL UP DURING THE BLE ATTACK.
Previously the memory guard in radioHasMemForBle() tore WiFi down to free DMA;
suspending the BLE API first is supposed to make that unnecessary.

Crash detection is unambiguous because the BLE API is not persisted across
reboots: a device that resumed correctly re-advertises as "Bruc", while one that
crashed or hung never does.

Usage: spike_swap.py [cycles] [attack_seconds]
"""
import asyncio
import subprocess
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Bruc"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04
AP_SSID = "BruceNet"


def ap_is_up() -> bool:
    """Scan for the device AP from the laptop's WiFi interface."""
    try:
        out = subprocess.run(
            ["nmcli", "-f", "SSID", "dev", "wifi", "list", "--rescan", "yes"],
            capture_output=True, text=True, timeout=60,
        ).stdout
    except Exception as exc:
        print(f"    (AP scan failed: {exc!r})")
        return False
    return AP_SSID in out


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self._evt = asyncio.Event()

    def on_notify(self, _sender, data: bytearray):
        self.buf += data
        if EOT in self.buf:
            self._evt.set()

    async def exec(self, cmd: str, timeout: float = 15.0) -> str:
        self.buf.clear()
        self._evt.clear()
        await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
        try:
            await asyncio.wait_for(self._evt.wait(), timeout)
        except asyncio.TimeoutError:
            return "<no EOT> " + self.buf.decode("utf-8", "replace")
        return self.buf.split(bytes([EOT]))[0].decode("utf-8", "replace").strip()


async def find(timeout=20.0):
    return await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)


async def cycle(n: int, attack_seconds: float) -> bool:
    print(f"\n===== CYCLE {n} =====")
    dev = await find()
    if dev is None:
        print("FAIL: 'Bruc' not advertising at cycle start")
        return False

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_notify)
        print(f"  connected, mtu={client.mtu_size}")

        pre = await link.exec("free")
        print(f"  pre-attack : {pre.splitlines()[0] if pre else '<empty>'}")

        # The spam blocks the firmware CLI, so no EOT-terminated reply is
        # expected for this verb — only the suspend warning that precedes it.
        print(f"  sending    : blespam fastpair_regular 30")
        link.buf.clear()
        try:
            await client.write_gatt_char(CLI, b"blespam fastpair_regular 30\n", response=True)
        except Exception as exc:
            print(f"  write raised: {exc!r}")

        warned = False
        for _ in range(50):
            await asyncio.sleep(0.2)
            if b"suspended" in link.buf:
                warned = True
                break
        print(f"  warning    : {'RECEIVED' if warned else 'NOT RECEIVED'} "
              f"{bytes(link.buf)[:90]!r}")

        for i in range(30):
            await asyncio.sleep(1.0)
            if not client.is_connected:
                print(f"  link dropped after ~{i + 1}s (expected — API suspended)")
                break
        else:
            print("  link still up (unexpected)")

    # THE decisive assertion: WiFi control path alive while the BLE attack runs.
    print("  scanning for the AP DURING the attack ...")
    ap_during = ap_is_up()
    print(f"  AP during attack: {'UP  <-- swap works' if ap_during else 'DOWN <-- swap failed'}")

    print(f"  waiting for resume (up to {attack_seconds:.0f}s) ...")
    t0 = time.time()
    dev = None
    while time.time() - t0 < attack_seconds:
        dev = await find(timeout=10.0)
        if dev is not None:
            break
    if dev is None:
        print(f"  FAIL: 'Bruc' never came back within {attack_seconds:.0f}s")
        return False
    print(f"  'Bruc' re-advertising after ~{time.time() - t0:.0f}s")

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_notify)
        post = await link.exec("free")
        ok = "HEAP" in post
        print(f"  post-resume: {post.splitlines()[0] if post else '<empty>'}")
        print(f"  CYCLE {n}: {'PASS' if (ok and ap_during) else 'FAIL'}")
        return ok and ap_during


async def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    attack_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
    results = []
    for i in range(1, cycles + 1):
        results.append(await cycle(i, attack_seconds))
        if not results[-1]:
            print("\nStopping: a cycle failed. Do not iterate blindly on this path.")
            break
    print(f"\n===== RESULT: {sum(results)}/{len(results)} cycles passed =====")
    return 0 if all(results) and results else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
