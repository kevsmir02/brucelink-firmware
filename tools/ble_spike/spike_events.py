#!/usr/bin/env python
"""BLE control-link spike, part 2 — event characteristic and EOT framing.

Verifies Tasks 6 and 7 plus the EOT response terminator:

  EOT    — responses end with 0x04, which (unlike the "# " prompt) cannot be
           produced by CLI text output, so it is a safe frame boundary.
  events — async {"id":..} JSON arrives on d555ed98 and never interleaves with
           CLI stdout on d555ed97.
  ids    — event IDs are monotonic and gap-free.
"""
import asyncio
import json
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Bruc"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
EVENTS = "d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9"
EOT = 0x04
TIMEOUT = 15.0


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = bytearray()
        self.chunks = []
        self.events = []
        self._ev_partial = ""
        self._done = asyncio.Event()

    def on_cli(self, _s, data: bytearray):
        self.chunks.append(len(data))
        self.buf += data
        if EOT in self.buf:
            self._done.set()

    def on_event(self, _s, data: bytearray):
        self._ev_partial += data.decode("utf-8", "replace")
        while "\n" in self._ev_partial:
            line, self._ev_partial = self._ev_partial.split("\n", 1)
            if line.strip():
                self.events.append(line.strip())

    async def exec(self, cmd: str):
        self.buf = bytearray()
        self.chunks = []
        self._done.clear()
        t0 = time.perf_counter()
        await self.client.write_gatt_char(CLI, (cmd + "\n").encode(), response=True)
        try:
            await asyncio.wait_for(self._done.wait(), TIMEOUT)
            timed_out = False
        except asyncio.TimeoutError:
            timed_out = True
        secs = time.perf_counter() - t0
        text = self.buf.split(bytes([EOT]))[0].decode("utf-8", "replace")
        return text, secs, timed_out


async def main():
    print(f"scanning for {DEVICE_NAME!r}…")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if not dev:
        print("NOT FOUND — is the BLE API on? (device: Config -> Toggle BLE API)")
        return 1

    async with BleakClient(dev, timeout=20.0) as client:
        svcs = client.services
        chars = {c.uuid.lower() for s in svcs for c in s.characteristics}
        print(f"\nevent characteristic present: {EVENTS in chars}")
        if EVENTS not in chars:
            print("  discovered:", sorted(chars))
            return 1

        link = Link(client)
        await client.start_notify(CLI, link.on_cli)
        await client.start_notify(EVENTS, link.on_event)
        print("subscribed to both characteristics")

        # --- EOT framing
        text, secs, timed_out = await link.exec("info")
        print(f"\n--- EOT framing")
        print(f"    terminated by EOT: {not timed_out}  ({secs * 1000:.0f} ms)")
        print(f"    response ends with prompt: {text.rstrip().endswith('#')}")
        print(f"    body: {text.strip().replace(chr(13) + chr(10), ' / ')[:150]}")

        # --- events arrived on their own characteristic
        await asyncio.sleep(0.5)
        print(f"\n--- event characteristic")
        print(f"    frames received: {len(link.events)}")
        for e in link.events[:6]:
            print(f"      {e}")

        # --- run a few commands, then check ID monotonicity
        before = len(link.events)
        for cmd in ("free", "info", "free"):
            await link.exec(cmd)
        await asyncio.sleep(0.7)
        new = link.events[before:]
        print(f"\n--- 3 more commands -> {len(new)} new event frames")

        ids, bad = [], []
        for raw in link.events:
            try:
                ids.append(json.loads(raw)["id"])
            except Exception:
                bad.append(raw)
        print(f"    parsed as JSON: {len(ids)}/{len(link.events)}")
        if bad:
            print(f"    UNPARSEABLE: {bad[:3]}")
        if ids:
            gaps = [(a, b) for a, b in zip(ids, ids[1:]) if b != a + 1]
            print(f"    id range: {ids[0]}..{ids[-1]}")
            print(f"    strictly monotonic +1: {not gaps}")
            if gaps:
                print(f"    gaps at: {gaps[:5]}")

        # --- separation: no event JSON leaked onto the CLI stream
        text, _, _ = await link.exec("info")
        leaked = '"type":"log"' in text or '{"id":' in text
        print(f"\n--- stream separation")
        print(f"    event JSON leaked into CLI response: {leaked}")

        await client.stop_notify(CLI)
        await client.stop_notify(EVENTS)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
