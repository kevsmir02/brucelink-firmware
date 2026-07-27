#!/usr/bin/env python
"""BLE control-link spike against the Bruce device.

Answers the questions the plan currently assumes rather than measures:

  D2   — does MTU negotiate above the default 23, and does the firmware
         actually chunk to the negotiated size?
  A1   — is the "# " prompt a reliable response terminator?
  corr — with no request IDs, do back-to-back commands stay correlated?
  perf — end-to-end latency per command after removing the pacing delay.

Runs from the laptop over BlueZ, so it needs no phone and no dev build.
The transport semantics it exercises are the same ones BleTransport will.
"""
import asyncio
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Bruc"
SVC = "4371ec0b-3d43-49f9-b731-7c72a4a7bb91"
CLI = "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9"
PROMPT = "# "
TIMEOUT = 15.0


class Link:
    def __init__(self, client):
        self.client = client
        self.buf = ""
        self.chunks = []
        self._evt = asyncio.Event()

    def on_notify(self, _sender, data: bytearray):
        self.chunks.append(len(data))
        self.buf += data.decode("utf-8", "replace")
        if self.buf.endswith(PROMPT):
            self._evt.set()

    async def exec(self, cmd: str, *, split_at: int | None = None):
        """Send cmd and collect until the prompt sentinel. Returns (text, secs, chunks)."""
        self.buf = ""
        self.chunks = []
        self._evt.clear()
        payload = (cmd + "\n").encode()

        t0 = time.perf_counter()
        if split_at is None:
            await self.client.write_gatt_char(CLI, payload, response=True)
        else:  # deliberately span two writes, the D1 regression case
            await self.client.write_gatt_char(CLI, payload[:split_at], response=True)
            await self.client.write_gatt_char(CLI, payload[split_at:], response=True)

        try:
            await asyncio.wait_for(self._evt.wait(), TIMEOUT)
        except asyncio.TimeoutError:
            return self.buf, time.perf_counter() - t0, self.chunks + ["TIMEOUT"]
        return self.buf, time.perf_counter() - t0, self.chunks


def show(label, text, secs, chunks):
    body = text[: -len(PROMPT)] if text.endswith(PROMPT) else text
    n = len(body.encode())
    sizes = sorted({c for c in chunks if isinstance(c, int)})
    print(f"\n--- {label}")
    print(f"    {secs * 1000:7.0f} ms | {n:5d} bytes | {len(chunks):3d} notifications | chunk sizes {sizes}")
    preview = body.strip().replace("\r\n", " / ")
    print(f"    {preview[:220]}{'…' if len(preview) > 220 else ''}")
    return body


async def main():
    print(f"scanning for {DEVICE_NAME!r}…")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if not dev:
        print(f"NOT FOUND. Is the BLE API on? (device menu: Config -> Toggle BLE API)")
        return 1
    print(f"found {dev.address}")

    async with BleakClient(dev, timeout=20.0) as client:
        link = Link(client)
        await client.start_notify(CLI, link.on_notify)

        # client.mtu_size is a placeholder until the real value is acquired from
        # BlueZ, so it reports 23 regardless. The authoritative evidence is the
        # observed notification chunk size, which the firmware sets to mtu - 3.
        try:
            await client._acquire_mtu()
        except Exception as e:  # noqa: BLE001 - best effort, chunk sizes are the real proof
            print(f"(could not acquire MTU from BlueZ: {e})")
        mtu = client.mtu_size
        print(f"\nMTU reported by BlueZ: {mtu}  ->  max notify payload {mtu - 3}")
        print("(plan assumes ~247; nRF Connect on Android measured 23)")
        print("Watch the chunk sizes below — those are the ground truth.")

        # 1. baseline
        show("info", *await link.exec("info"))

        # 2. large payload — the D2 case. chunk sizes reveal the real MTU in use.
        body = show("systeminfo", *await link.exec("systeminfo"))
        ok_json = body.strip().endswith("}")
        print(f"    JSON complete: {ok_json}")

        # 3. D1 regression — one command deliberately split across two writes.
        # Compare against the unsplit reply: identical output means the ring
        # reassembled it. (show() returns the body with the prompt stripped —
        # checking the raw text here would always fail on the trailing "# ".)
        split_body = show("systeminfo (split 'system' + 'info\\n')", *await link.exec("systeminfo", split_at=6))
        print(f"    no spurious ERROR: {'ERROR' not in split_body}")
        print(f"    JSON complete:     {split_body.strip().endswith('}')}")
        print(f"    same length as unsplit reply: {len(split_body) == len(body)}")

        # 4. correlation — back-to-back commands with no request IDs
        print("\n--- correlation: 5 back-to-back commands")
        good = 0
        for i in range(5):
            cmd = "info" if i % 2 == 0 else "free"
            body, secs, _ = await link.exec(cmd)
            expect = "Bruce" if cmd == "info" else "Total heap"
            hit = expect in body
            good += hit
            print(f"    {i + 1}. {cmd:10s} {secs * 1000:6.0f} ms  matched={hit}")
        print(f"    {good}/5 responses matched their request")

        # 5. a command longer than 20 bytes in a SINGLE write — only possible
        #    if MTU actually negotiated up. This is the D2 acceptance test.
        long_cmd = "blescan 1"  # short + harmless; padded check below
        probe = "systeminfo" + " " * 15  # 25 bytes, >20, still parses as systeminfo
        body, secs, chunks = await link.exec(probe)
        show(f"long single write ({len(probe) + 1} bytes)", body, secs, chunks)
        print(f"    survived a >20-byte single write: {'ERROR' not in body}")

        await client.stop_notify(CLI)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
