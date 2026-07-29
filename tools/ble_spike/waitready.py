import asyncio,sys,time
from bleak import BleakClient, BleakScanner
CLI="d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
async def once():
    dev=await BleakScanner.find_device_by_name("Bruc",timeout=8.0)
    if not dev: return None
    try:
        async with BleakClient(dev) as c:
            buf=bytearray(); ev=asyncio.Event()
            def on(_s,d):
                buf.extend(d)
                if 0x04 in buf: ev.set()
            await c.start_notify(CLI,on)
            await c.write_gatt_char(CLI,b"uptime\n",response=True)
            await asyncio.wait_for(ev.wait(),6)
            return bytes(buf).split(b'\x04')[0].decode('utf-8','replace').strip()
    except Exception: return None
async def main():
    deadline=time.time()+float(sys.argv[1])
    while time.time()<deadline:
        r=await once()
        if r:
            print(f"DEVICE READY — {r.splitlines()[0]}",flush=True); return 0
        print("  ...waiting for reset",flush=True)
        await asyncio.sleep(2)
    print("TIMEOUT waiting for device"); return 1
sys.exit(asyncio.run(main()))
