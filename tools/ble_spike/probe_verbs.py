#!/usr/bin/env python
"""Probe device CLI verbs over BLE and report which respond.

DELIBERATELY PARTIAL. Three classes of verb are excluded and must stay excluded
from any blind sweep:

  destructive   factory_reset, erase, rm, rmdir, remove, reboot, poweroff, off,
                sleep, reset, write, save, copy, rename, mkdir, add
  blocking      evilportal, karma, deauth, sniffer, blespam, blesniffer,
                reverseshell, pwngrid, listen, responder — these hold the
                firmware CLI until they finish or are escaped on the device
  menu-opening  rf, ir, rfid, badusb, play, loader, scan, clone, tx, rx, ymodem
                and friends open an on-device menu and wait for a physical
                button press, which would strand the CLI with no way back from
                the laptop

What remains is the read-only/informational surface, which is what can honestly
be asserted about without a human at the device.
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04

SAFE_VERBS = [
    "uptime",
    "date",
    "free",
    "info",
    "systeminfo",
    "optionsJSON",
    "i2c",
    "ls",
    "gpio",
    "txp",
]


async def main():
    dev = await BleakScanner.find_device_by_name("Bruc", timeout=15.0)
    if dev is None:
        print("FAIL: 'Bruc' not advertising — the BLE API is off")
        return 1

    async with BleakClient(dev) as client:
        buf = bytearray()
        evt = asyncio.Event()

        def on_notify(_s, data):
            buf.extend(data)
            if EOT in buf:
                evt.set()

        await client.start_notify(CLI, on_notify)
        print(f"connected mtu={client.mtu_size}\n")
        print(f"{'verb':<14} {'ms':>7}  result")
        print("-" * 72)

        results = {}
        for verb in SAFE_VERBS:
            buf.clear()
            evt.clear()
            t0 = time.time()
            try:
                await client.write_gatt_char(CLI, (verb + "\n").encode(), response=True)
                await asyncio.wait_for(evt.wait(), 30)
                out = buf.split(bytes([EOT]))[0].decode("utf-8", "replace").strip()
                ms = (time.time() - t0) * 1000
                first = out.replace("\r\n", " | ").replace("\n", " | ")[:64]
                ok = bool(out) and "Command not found" not in out
                results[verb] = "OK" if ok else "UNKNOWN-VERB"
                print(f"{verb:<14} {ms:>7.0f}  {'OK  ' if ok else 'ERR '} {first}")
            except asyncio.TimeoutError:
                results[verb] = "TIMEOUT"
                print(f"{verb:<14} {'--':>7}  TIMEOUT (no EOT in 30s)")

        print("-" * 72)
        ok = sum(1 for v in results.values() if v == "OK")
        print(f"{ok}/{len(SAFE_VERBS)} responded")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
