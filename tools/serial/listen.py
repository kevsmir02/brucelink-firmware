#!/usr/bin/env python
"""Passive listen on a serial port without sending anything.

Opens the port holding DTR/RTS de-asserted, waits BRUCE_SETTLE seconds,
then prints everything received for BRUCE_WINDOW seconds. Useful to see
what the device emits on its own after the boot-from-port-open, and to
find the CLI prompt that cli.py needs to land after.
"""
import os
import sys
import time
import serial

PORT = os.environ.get("BRUCE_PORT", "/dev/ttyACM1")
BAUD = 115200
SETTLE = float(os.environ.get("BRUCE_SETTLE", "30"))
WINDOW = float(os.environ.get("BRUCE_WINDOW", "15"))

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()
print(f"--- opened {PORT}, settling {SETTLE:.0f}s ---", flush=True)
time.sleep(SETTLE)
ser.reset_input_buffer()
print(f"--- listening {WINDOW:.0f}s ---", flush=True)
deadline = time.time() + WINDOW
total = 0
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        total += len(data)
        sys.stdout.write(data.decode("utf-8", errors="replace"))
        sys.stdout.flush()
ser.close()
print(f"\n--- captured {total} bytes ---", flush=True)