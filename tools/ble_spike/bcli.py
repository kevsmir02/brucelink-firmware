"""Batch CLI client. Discovers by SERVICE UUID, never by name.

usage: bcli.py [--timeout N] <verb> [<verb> ...]
       bcli.py --file verbs.txt      (one verb per line, # comments ignored)

Prints one block per verb with elapsed ms, byte count, and whether the 0x04 EOT
terminator actually arrived — a missing EOT is a finding, not a nuisance.
"""
import asyncio, sys, time
from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04

args = sys.argv[1:]
timeout = 25.0
if args and args[0] == "--timeout":
    timeout = float(args[1]); args = args[2:]
if args and args[0] == "--file":
    verbs = [l.strip() for l in open(args[1]) if l.strip() and not l.strip().startswith("#")]
else:
    verbs = args


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
        print("FAIL: no device advertising service UUID", SVC)
        return 1
    print(f"# connected via service UUID -> {dev.address}\n")
    async with BleakClient(dev) as c:
        buf = bytearray()
        evt = asyncio.Event()

        def on(_s, d):
            buf.extend(d)
            if EOT in buf:
                evt.set()

        await c.start_notify(CLI, on)
        for verb in verbs:
            buf.clear(); evt.clear(); t0 = time.time()
            await c.write_gatt_char(CLI, (verb + "\n").encode(), response=True)
            timed_out = False
            try:
                await asyncio.wait_for(evt.wait(), timeout)
            except asyncio.TimeoutError:
                timed_out = True
            ms = (time.time() - t0) * 1000
            raw = bytes(buf)
            got_eot = EOT in raw
            txt = raw.split(b"\x04")[0].decode("utf-8", "replace")
            flag = "  *** NO EOT / TIMEOUT ***" if timed_out or not got_eot else ""
            print(f"===== $ {verb}   ({ms:.0f} ms, {len(raw)} bytes, eot={got_eot}){flag}")
            print(txt.rstrip() if txt.strip() else "(empty reply)")
            print()
            if timed_out:
                print("!! stopping batch: device did not terminate the reply "
                      "(likely a blocking verb). Remaining verbs not sent:", verbs[verbs.index(verb)+1:])
                break
        await c.stop_notify(CLI)
    return 0


sys.exit(asyncio.run(main()))
