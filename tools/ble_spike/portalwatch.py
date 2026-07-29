"""Dispatch a blocking attack verb and keep listening on the EVENT characteristic.

The CLI characteristic goes silent for the whole life of a blocking verb, but the
event characteristic is fed from pushWsEvent on other tasks, so it keeps arriving.
This prints every event frame with a timestamp relative to dispatch.

usage: portalwatch.py "<verb>" <listen_seconds>
"""
import asyncio, sys, time
from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EVENTS = "d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"

VERB = sys.argv[1]
LISTEN = float(sys.argv[2])


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
    t0 = time.time()
    evbuf = bytearray()
    dropped = asyncio.Event()

    async with BleakClient(dev, disconnected_callback=lambda _c: dropped.set()) as c:
        def on_event(_s, d):
            evbuf.extend(d)
            while b"\n" in evbuf:
                line, _, rest = evbuf.partition(b"\n")
                del evbuf[:]
                evbuf.extend(rest)
                t = line.decode("utf-8", "replace").strip()
                if t:
                    print(f"+{time.time()-t0:7.2f}s EVENT {t}", flush=True)

        def on_cli(_s, d):
            t = bytes(d).replace(b"\x04", b"").decode("utf-8", "replace").strip()
            if t:
                print(f"+{time.time()-t0:7.2f}s CLI   {t!r}", flush=True)

        await c.start_notify(EVENTS, on_event)
        await c.start_notify(CLI, on_cli)
        await asyncio.sleep(0.5)

        print(f"+{time.time()-t0:7.2f}s ---> dispatching {VERB!r}", flush=True)
        await c.write_gatt_char(CLI, (VERB + "\n").encode(), response=True)

        try:
            await asyncio.wait_for(dropped.wait(), LISTEN)
            print(f"+{time.time()-t0:7.2f}s *** BLE LINK DROPPED ***", flush=True)
        except asyncio.TimeoutError:
            print(f"+{time.time()-t0:7.2f}s (listen window ended, link still up)", flush=True)
    return 0


sys.exit(asyncio.run(main()))
