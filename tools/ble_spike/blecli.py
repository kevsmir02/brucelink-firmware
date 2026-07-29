import asyncio, sys, time
from bleak import BleakClient, BleakScanner
CLI="d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"; EOT=0x04

async def main():
    dev = await BleakScanner.find_device_by_name("Bruc", timeout=15.0)
    if dev is None:
        print("FAIL: 'Bruc' not advertising"); return 1
    async with BleakClient(dev) as c:
        buf=bytearray(); evt=asyncio.Event()
        def on(_s,d):
            buf.extend(d)
            if EOT in buf: evt.set()
        await c.start_notify(CLI,on)
        for verb in sys.argv[1:]:
            buf.clear(); evt.clear(); t0=time.time()
            await c.write_gatt_char(CLI,(verb+"\n").encode(),response=True)
            try: await asyncio.wait_for(evt.wait(),25)
            except asyncio.TimeoutError: pass
            ms=(time.time()-t0)*1000
            txt=bytes(buf).split(b'\x04')[0].decode('utf-8','replace')
            print(f"\n===== $ {verb}   ({ms:.0f} ms, {len(buf)} bytes) =====")
            print(txt.rstrip())
        await c.stop_notify(CLI)
    return 0
sys.exit(asyncio.run(main()))
