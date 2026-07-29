#!/usr/bin/env python3
"""Prove the headless Evil Portal does not hold the serial task.

The load-bearing assertion is step 2: today the portal blocks the CLI for its
entire life, so a reply to `uptime` while a portal is up means it is genuinely
headless. Everything else is secondary.

Discovery is by service UUID, never by name — name discovery has produced four
false "device is bricked" conclusions on this project.

Usage:  python3 portal_bg.py [--ssid PortalTest] [--channel 6] [--duration 300]
        python3 portal_bg.py --cap-only [--cap-seconds 45]

--cap-only is unattended and exercises the duration cap, which is the only
recovery path that survives `ble api off` and the only caller of
stopPortal(announceOnCli=False). It also watches for unsolicited bytes on the
CLI characteristic, because a cap that announced itself there would land inside
another command's reply and break its 0x04 EOT framing.
"""
import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EVT = "d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self.done = asyncio.Event()
        self.events = []
        self.awaiting = False
        self.stray = bytearray()

    def on_cli(self, _h, data):
        # Bytes arriving while no command is outstanding are unsolicited. Nothing
        # in the design may write to the CLI characteristic unprompted.
        if not self.awaiting:
            self.stray.extend(data)
            print(f"    [STRAY CLI BYTES] {bytes(data)!r}")
            return
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
        self.awaiting = True
        try:
            await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
            try:
                await asyncio.wait_for(self.done.wait(), timeout)
            except asyncio.TimeoutError:
                return None  # no EOT: blocked, or the reply could not be allocated
            return self.buf.replace(bytes([EOT]), b"").decode("utf-8", "replace").strip()
        finally:
            self.awaiting = False


async def connect():
    print(f"scanning for service {SVC} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC.lower() in [u.lower() for u in ad.service_uuids], timeout=15.0
    )
    if dev is None:
        print("FAIL: no device advertising the Bruce service UUID")
        return None
    print(f"found {dev.address}")
    return dev


async def run_cap_only(args, link):
    cap = args.cap_seconds
    print(f"\n[1] start headless portal '{args.ssid}' ch{args.channel} cap {cap}s")
    reply = await link.send(f"evilportal {args.ssid} {args.channel} -bg -duration {cap}")
    print(f"    {reply!r}")
    if reply is None or "ERROR" in (reply or ""):
        print("FAIL: portal did not start")
        return 1

    print(f"\n[2] poll -status until the cap fires (deadline {cap + 60}s)")
    t0 = time.time()
    stopped_at = None
    while time.time() - t0 < cap + 60:
        await asyncio.sleep(5.0)
        st = await link.send("evilportal -status")
        elapsed = time.time() - t0
        print(f"    +{elapsed:6.1f}s  {st!r}")
        if st is None:
            print("FAIL: no reply to -status; the serial task is held")
            return 1
        if "stopped" in st:
            stopped_at = elapsed
            break

    if stopped_at is None:
        print(f"FAIL: portal still running past cap+60s. The cap did not fire.")
        return 1
    print(f"    PASS: portal self-stopped at +{stopped_at:.1f}s (cap {cap}s)")

    # A cap that wrote to the CLI characteristic would corrupt the next reply.
    print("\n[3] framing intact after the cap fired?")
    reply = await link.send("uptime")
    print(f"    {reply!r}")
    if reply is None:
        print("FAIL: no reply after the cap fired")
        return 1
    if not reply.startswith("Uptime:"):
        print("FAIL: reply is not a clean uptime — the cap wrote to the CLI stream")
        return 1
    if link.stray:
        print(f"FAIL: {len(link.stray)} unsolicited byte(s) seen on the CLI characteristic")
        return 1
    print("    PASS: clean reply, zero stray CLI bytes")
    return 0


async def run_interactive(args, link):
    print(f"\n[1] start headless portal '{args.ssid}' ch{args.channel} cap {args.duration}s")
    reply = await link.send(
        f"evilportal {args.ssid} {args.channel} -bg -duration {args.duration}"
    )
    print(f"    {reply!r}")
    if reply is None or "ERROR" in (reply or ""):
        print("FAIL: portal did not start")
        return 1

    # The reply's "started" is necessary but not sufficient — confirm the AP is
    # actually beaconing from another machine before trusting it.
    print("    NOTE: confirm the SSID is visible in a scan from another machine.")

    # The whole point of the change. Before it, this call could not be answered.
    print("\n[2] send `uptime` over BLE while the portal is up  <-- load-bearing")
    reply = await link.send("uptime", timeout=15.0)
    if reply is None:
        print("FAIL: no reply. The serial task is still held; the portal is NOT headless.")
        return 1
    print(f"    PASS: {reply!r}")

    print("\n[3] status")
    print(f"    {await link.send('evilportal -status')!r}")

    if args.duration:
        print(
            f"\n    WARNING: the cap is {args.duration}s. If you take longer than that,"
            f"\n    the portal self-stops and step 5's -off will correctly report"
            f"\n    'no background portal running' — that is the cap working, not a"
            f"\n    failed stop. Use --duration 0 to disable the cap for this run."
        )
    print(f"\n[4] join AP '{args.ssid}' from another machine, browse 192.168.4.1,")
    print("    submit credentials, then press Enter here.")
    await asyncio.get_running_loop().run_in_executor(None, input)
    print(f"    {await link.send('evilportal -status')!r}")
    caps = [e for e in link.events if "captured" in e]
    print(f"    capture events seen: {len(caps)}")

    print("\n[5] stop over BLE")
    reply = await link.send("evilportal -off")
    print(f"    {reply!r}")

    print("\n[6] confirm BLE still alive after stop")
    reply = await link.send("uptime")
    if reply is None:
        print("FAIL: no reply after stop")
        return 1
    print(f"    PASS: {reply!r}")

    print("\n[7] -off with nothing running should say so, not claim success")
    reply = await link.send("evilportal -off")
    print(f"    {reply!r}")
    if reply is None or "no background portal" not in reply:
        print("FAIL: expected 'no background portal running'")
        return 1
    print("    PASS")

    if link.stray:
        print(f"\nFAIL: {len(link.stray)} unsolicited byte(s) seen on the CLI characteristic")
        return 1
    return 0


async def main(args):
    dev = await connect()
    if dev is None:
        return 1

    async with BleakClient(dev) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_cli)
        await client.start_notify(EVT, link.on_evt)
        rc = await (run_cap_only(args, link) if args.cap_only else run_interactive(args, link))

    print("\ndone" if rc == 0 else "\nFAILED")
    return rc


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--ssid", default="PortalTest")
    p.add_argument("--channel", type=int, default=6)
    p.add_argument("--duration", type=int, default=300)
    p.add_argument("--cap-only", action="store_true", help="unattended duration-cap test")
    p.add_argument("--cap-seconds", type=int, default=45)
    a = p.parse_args()
    if " " in a.ssid:
        sys.exit("SSID must not contain spaces — SimpleCLI splits the command on words")
    sys.exit(asyncio.run(main(a)))
