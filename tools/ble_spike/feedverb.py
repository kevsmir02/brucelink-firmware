"""Test `storage write`, which puts the CLI into a line-consuming mode until an
`EOF` line or 5 s of silence (helpers.cpp:44,51).

usage: writetest.py <path> <declared_size> <line> [<line> ...]
"""
import asyncio, sys, time
from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04

path = sys.argv[1]
lines = sys.argv[2:]


async def find():
    found = {}

    def cb(dev, adv):
        if any(SVC in u.lower() for u in adv.service_uuids):
            found.setdefault(dev.address, dev)

    sc = BleakScanner(detection_callback=cb)
    await sc.start()
    for _ in range(40):
        if found:
            break
        await asyncio.sleep(0.5)
    await sc.stop()
    return next(iter(found.values())) if found else None


async def main():
    dev = await find()
    if dev is None:
        print("FAIL: device not found"); return 1
    async with BleakClient(dev) as c:
        buf = bytearray(); evt = asyncio.Event()

        def on(_s, d):
            buf.extend(d)
            if EOT in buf:
                evt.set()

        await c.start_notify(CLI, on)
        t0 = time.time()
        await c.write_gatt_char(CLI, f"{path}\n".encode(), response=True)
        await asyncio.sleep(0.6)
        print(f"after verb ({(time.time()-t0)*1000:.0f} ms): {bytes(buf).decode('utf-8','replace')!r}")

        for l in lines:
            await c.write_gatt_char(CLI, (l + "\n").encode(), response=True)
            await asyncio.sleep(0.15)
        await c.write_gatt_char(CLI, b"EOF\n", response=True)

        try:
            await asyncio.wait_for(evt.wait(), 20)
        except asyncio.TimeoutError:
            print("!! no EOT within 20 s after EOF line")
        ms = (time.time() - t0) * 1000
        print(f"\n===== full reply ({ms:.0f} ms, {len(buf)} bytes, eot={EOT in bytes(buf)})")
        print(bytes(buf).split(b'\x04')[0].decode('utf-8', 'replace').rstrip())
        await c.stop_notify(CLI)
    return 0


sys.exit(asyncio.run(main()))
