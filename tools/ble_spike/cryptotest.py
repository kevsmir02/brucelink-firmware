"""Falsifiable test of the encrypt/decrypt round-trip hypothesis.

Hypothesis: decrypt succeeds iff every hex token on the Data: line is 2 chars.
encryptString writes String(byte, HEX) with no zero-pad (passwords.cpp:162);
readDecryptedFile parses on a fixed 3-char stride (passwords.cpp:113).

Encrypts several payloads, reads back each Data line, predicts from the token
widths, then actually decrypts and compares prediction against outcome.
"""
import asyncio, sys
from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04
PW = "hunter2"
PAYLOADS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"]


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

        async def send(line, wait=True, t=20):
            buf.clear(); evt.clear()
            await c.write_gatt_char(CLI, (line + "\n").encode(), response=True)
            if not wait:
                await asyncio.sleep(0.5); return bytes(buf).decode("utf-8", "replace")
            try:
                await asyncio.wait_for(evt.wait(), t)
            except asyncio.TimeoutError:
                pass
            return bytes(buf).split(b"\x04")[0].decode("utf-8", "replace")

        rows = []
        for i, p in enumerate(PAYLOADS):
            path = f"/bl_ct{i}.enc"
            await send(f"encrypt {path} {PW}", wait=False)
            await asyncio.sleep(0.3)
            buf.clear(); evt.clear()
            await c.write_gatt_char(CLI, (p + "\n").encode(), response=True)
            await asyncio.sleep(0.15)
            await c.write_gatt_char(CLI, b"EOF\n", response=True)
            try:
                await asyncio.wait_for(evt.wait(), 15)
            except asyncio.TimeoutError:
                pass

            body = await send(f"cat {path}")
            data = ""
            for l in body.splitlines():
                if l.startswith("Data:"):
                    data = l[len("Data:"):].strip()
            toks = data.split()
            short = [t for t in toks if len(t) != 2]
            predict_ok = len(short) == 0

            out = await send(f"decrypt {path} {PW}")
            plain = out.split("#")[0].strip()
            actual_ok = plain == p

            rows.append((p, len(toks), short, predict_ok, actual_ok, plain))
            await send(f"rm {path}")

        print(f"{'payload':9} {'bytes':5} {'short tokens':16} {'predict':8} {'actual':7} match")
        agree = 0
        for p, n, short, pr, ac, plain in rows:
            agree += (pr == ac)
            print(f"{p:9} {n:<5} {str(short):16} {str(pr):8} {str(ac):7} "
                  f"{'AGREE' if pr == ac else '*** DISAGREE ***'}   got={plain!r}")
        print(f"\nhypothesis agreed with outcome in {agree}/{len(rows)} cases")
        await c.stop_notify(CLI)
    return 0


sys.exit(asyncio.run(main()))
