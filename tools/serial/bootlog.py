#!/usr/bin/env python
"""Reset the board into NORMAL boot (not download mode) and capture the log.

RTS drives EN (reset), DTR drives GPIO0 (boot select), via the usual
two-transistor auto-reset circuit. Holding DTR de-asserted keeps GPIO0 high,
so releasing EN boots the application image rather than the ROM downloader.
Mirrors esptool's polarity convention: ser.dtr/ser.rts True == signal asserted.
"""
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200
WINDOW = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

ser = serial.Serial(PORT, BAUD, timeout=0.1)

# Normal-boot reset: GPIO0 stays high throughout, EN pulsed low then high.
ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.reset_input_buffer()
ser.rts = False

print(f"--- reset issued, capturing {WINDOW:.0f}s ---", flush=True)
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
