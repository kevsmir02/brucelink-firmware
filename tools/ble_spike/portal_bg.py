#!/usr/bin/env python3
"""Prove the headless Evil Portal does not hold the serial task.

The load-bearing assertion is step 2: today the portal blocks the CLI for its
entire life, so a reply to `uptime` while a portal is up means it is genuinely
headless. Everything else is secondary.

Discovery is by service UUID, never by name — name discovery has produced four
false "device is bricked" conclusions on this project.

Usage:  python3 portal_bg.py [--ssid PortalTest] [--channel 6] [--duration 300]
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-3d43-49f9-b731-7c72a4a7bb91"
EVT = "d555ed98-3d43-49f9-b731-7c72a4a7bb91"
EOT = 0x04


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self.done = asyncio.Event()
        self.events = []

    def on_cli(self, _h, data):
        self.buf.extend(data)
        if EOT in data:
            self.done.set()

    def on_evt(self, _h, data):
        text = data.decode("utf-8", "replace").strip()
        self.events.append(text)
        print(f"    [event] {text}")

    async def send(self, cmd, timeout=20.0):
        self.buf.clear()
        self.done.clear()
        await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=False)
        try:
            await asyncio.wait_for(self.done.wait(), timeout)
        except asyncio.TimeoutError:
            return None  # no EOT: either blocked or the reply could not be allocated
        return self.buf.replace(bytes([EOT]), b"").decode("utf-8", "replace").strip()


async def main(args):
    print(f"scanning for service {SVC} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC.lower() in [u.lower() for u in ad.service_uuids], timeout=15.0
    )
    if dev is None:
        print("FAIL: no device advertising the Bruce service UUID")
        return 1
    print(f"found {dev.address}")

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_cli)
        await client.start_notify(EVT, link.on_evt)

        print(f"\n[1] start headless portal '{args.ssid}' ch{args.channel} cap {args.duration}s")
        reply = await link.send(
            f"evilportal {args.ssid} {args.channel} -bg -duration {args.duration}"
        )
        print(f"    {reply!r}")
        if reply is None or "ERROR" in (reply or ""):
            print("FAIL: portal did not start")
            return 1

        # The whole point of the change. Before it, this call could not be answered.
        print("\n[2] send `uptime` over BLE while the portal is up  <-- load-bearing")
        reply = await link.send("uptime", timeout=15.0)
        if reply is None:
            print("FAIL: no reply. The serial task is still held; the portal is NOT headless.")
            return 1
        print(f"    PASS: {reply!r}")

        print("\n[3] status")
        print(f"    {await link.send('evilportal -status')!r}")

        print(f"\n[4] join AP '{args.ssid}' from another machine, browse 192.168.4.1,")
        print("    submit credentials, then press Enter here.")
        await asyncio.get_event_loop().run_in_executor(None, input)
        print(f"    {await link.send('evilportal -status')!r}")
        caps = [e for e in link.events if "captured" in e]
        print(f"    capture events seen: {len(caps)}")

        print("\n[5] stop over BLE")
        print(f"    {await link.send('evilportal -off')!r}")

        print("\n[6] confirm BLE still alive after stop")
        reply = await link.send("uptime")
        if reply is None:
            print("FAIL: no reply after stop")
            return 1
        print(f"    PASS: {reply!r}")

        print("\n[7] -off with nothing running should say so, not claim success")
        print(f"    {await link.send('evilportal -off')!r}")

    print("\ndone")
    return 0


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--ssid", default="PortalTest")
    p.add_argument("--channel", type=int, default=6)
    p.add_argument("--duration", type=int, default=300)
    sys.exit(asyncio.run(main(p.parse_args())))
