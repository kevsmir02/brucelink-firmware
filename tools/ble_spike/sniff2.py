import asyncio,sys,time
from bleak import BleakClient, BleakScanner
CLI="d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
VERB=sys.argv[1]; SCAN=float(sys.argv[2])
CO={6:'Microsoft',76:'Apple',117:'Samsung',224:'Google',771:'*** 0x0303 BOGUS ***',19456:'(0x4C00)'}
seen={}; order=[]
def cb(dev, adv):
    if dev.address not in seen:
        seen[dev.address]={'name':adv.local_name,'sd':dict(adv.service_data),'md':dict(adv.manufacturer_data)}
        order.append(dev.address)
async def main():
    d=await BleakScanner.find_device_by_name("Bruc",timeout=20.0)
    if not d: print("FAIL: not advertising"); return 2
    disc=asyncio.Event()
    async with BleakClient(d,disconnected_callback=lambda _c: disc.set()) as c:
        await c.write_gatt_char(CLI,(VERB+"\n").encode(),response=True)
        try: await asyncio.wait_for(disc.wait(),20)
        except asyncio.TimeoutError: pass
    sc=BleakScanner(detection_callback=cb)
    await sc.start(); await asyncio.sleep(SCAN); await sc.stop()
    print(f"\n===== {VERB} — {len(seen)} distinct addresses in {SCAN:.0f}s =====")
    from collections import Counter
    cos=Counter(); svc=Counter()
    for a in order:
        e=seen[a]
        for cid in e['md']: cos[cid]+=1
        for k in e['sd']: svc[k[4:8].lower()]+=1
    print("manufacturer company IDs seen:")
    for cid,n in cos.most_common(): print(f"   {cid:6d} 0x{cid:04X}  {CO.get(cid,'?'):22s} x{n}")
    print("service-data UUIDs seen:")
    for u,n in svc.most_common(): print(f"   0x{u}  x{n}")
    print("-- samples --")
    for a in order[:14]:
        e=seen[a]
        sd={k[4:8]:v.hex() for k,v in e['sd'].items()}
        md={f"{k}({CO.get(k,'?')})":v.hex() for k,v in e['md'].items()}
        print(f"  {a} name={e['name']!r} sd={sd} md={md}")
    return 0
sys.exit(asyncio.run(main()))
