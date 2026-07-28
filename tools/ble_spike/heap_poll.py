#!/usr/bin/env python
"""Poll the `free` verb over the BLE control link and print the heap series.

Complements the [RAMLOG] stream on UART0: this path proves the same numbers are
reachable over BLE, which is what matters during a WiFi attack when the AP — and
with it the HTTP transport — is gone.

Commands are newline-framed (see firmware a512e3c1) and each response is
terminated by BLE_RESPONSE_EOT rather than the "# " prompt, because any output
line beginning with "# " would otherwise truncate the reply (9c7f7260).

Usage: heap_poll.py [samples] [interval_seconds]
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Bruc"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04
TIMEOUT = 20.0


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self._evt = asyncio.Event()

    def on_notify(self, _sender, data: bytearray):
        self.buf += data
        if EOT in self.buf:
            self._evt.set()

    async def exec(self, cmd: str) -> str:
        self.buf.clear()
        self._evt.clear()
        await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
        try:
            await asyncio.wait_for(self._evt.wait(), TIMEOUT)
        except asyncio.TimeoutError:
            return f"<timeout> partial={self.buf.decode('utf-8', 'replace')!r}"
        return self.buf.split(bytes([EOT]))[0].decode("utf-8", "replace").strip()


async def main():
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if dev is None:
        print(f"FAIL: no BLE device named {DEVICE_NAME!r} advertising.")
        print("The BLE API is not persisted across reboots — toggle it on the device.")
        return 1

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_notify)
        print(f"connected mtu={client.mtu_size}")
        for i in range(samples):
            t0 = time.time()
            out = await link.exec("free")
            print(f"[{i}] {(time.time() - t0) * 1000:6.0f}ms  {out}")
            if i + 1 < samples:
                await asyncio.sleep(interval)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
