"""ISSUE-9 verifier: does the device come back with its NAME, SERVICE UUID and
FACTORY BT MAC after a blespam run?

Deliberately discovers by SERVICE UUID, never by name, so a missing name is
reported as a finding rather than masquerading as a missing device (the harness
failure that produced four bogus "device is bricked" conclusions on 2026-07-29).

usage: issue9_verify.py <spam_verb> [settle_seconds]
"""
import asyncio, sys
from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04

VERB = sys.argv[1]
SETTLE = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0


async def find_by_uuid(timeout):
    """Scan for anything advertising the Bruce service UUID."""
    found = {}

    def cb(dev, adv):
        if any(SVC in u.lower() for u in adv.service_uuids):
            found.setdefault(dev.address, (dev, adv))

    sc = BleakScanner(detection_callback=cb)
    await sc.start()
    for _ in range(int(timeout * 2)):
        if found:
            break
        await asyncio.sleep(0.5)
    await sc.stop()
    return found


async def snapshot(label, timeout=20.0):
    found = await find_by_uuid(timeout)
    print(f"\n--- {label} ---")
    if not found:
        print("  NO advert carrying the Bruce service UUID")
        return None
    for addr, (dev, adv) in found.items():
        print(f"  addr={addr}  name={adv.local_name!r}  uuids={[u[:8] for u in adv.service_uuids]}")
    return list(found.items())[0]


async def main():
    before = await snapshot("BEFORE spam (fresh state)")
    if not before:
        print("FAIL: device not advertising the service UUID before the test")
        return 2
    addr_before, (dev_before, adv_before) = before

    disc = asyncio.Event()
    async with BleakClient(dev_before, disconnected_callback=lambda _c: disc.set()) as c:
        await c.write_gatt_char(CLI, (VERB + "\n").encode(), response=True)
        print(f"\n  sent: {VERB}   (waiting for the BLE API to suspend itself...)")
        try:
            await asyncio.wait_for(disc.wait(), 25)
            print("  link dropped — spam has the radio")
        except asyncio.TimeoutError:
            print("  link never dropped (verb may have finished inline)")

    print(f"  settling {SETTLE:.0f}s for the spam to finish and the BLE API to resume...")
    await asyncio.sleep(SETTLE)

    after = await snapshot("AFTER spam", timeout=30.0)
    print("\n===== RESULT =====")
    if not after:
        print(f"  {VERB}: FAIL — no service-UUID advert after the run")
        return 1
    addr_after, (_d, adv_after) = after
    name_ok = adv_after.local_name == "Bruc"
    mac_ok = addr_after.lower() == addr_before.lower()
    print(f"  name  : {adv_after.local_name!r}  {'PASS' if name_ok else 'FAIL (expected Bruc)'}")
    print(f"  uuid  : present  PASS  (that is how it was discovered)")
    print(f"  bt mac: {addr_before} -> {addr_after}  {'PASS (restored)' if mac_ok else 'FAIL (not restored)'}")
    return 0 if (name_ok and mac_ok) else 1


sys.exit(asyncio.run(main()))
