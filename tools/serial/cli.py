#!/usr/bin/env python
"""Send CLI commands to a running Bruce device over serial and print replies.

Framing per vendor/bruce-companion-api.md 2.1: commands are newline-terminated,
replies stream back and end with a human prompt "# " then a single 0x04 EOT.
Read until EOT -- never treat "# " as the terminator.

Caveat on the smoochiee-board (and likely other ARDUINO_USB_CDC_ON_BOOT boards):
the CLI does NOT listen on native USB when bleApiAutoStart=1 — `serialDevice`
rebinds to BLE at boot (ble_api.cpp:63), so bytes sent to /dev/ttyACM1 reach
nobody. Use tools/ble_spike/bcli.py (BLE) for the default config; this script
only works once bleApiAutoStart=0 or after a BLE API end() rebinds back to USB.

Opening either /dev/ttyACM port resets the board (auto-reset circuit + CDC
connect); the DTR/RTS-hold-low attempt here does not prevent it on this board.
BRUCE_SETTLE must be >= ~25s to clear the ~10-30s boot banner before sending.
"""
import os
import sys
import time

import serial

PORT = os.environ.get("BRUCE_PORT", "/dev/ttyACM0")
BAUD = 115200
EOT = 0x04
PER_CMD_TIMEOUT = float(os.environ.get("BRUCE_TIMEOUT", "30"))

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
# Hold both lines de-asserted across open so the auto-reset circuit never
# pulses EN -- we want the log of the device that is already running.
ser.dtr = False
ser.rts = False
ser.open()
# Opening the port resets the board (the kernel asserts DTR/RTS before pyserial
# can hold them low), so wait out the ~11s boot before sending anything --
# otherwise the command lands mid-setup and the "reply" is just the boot log.
settle = float(os.environ.get("BRUCE_SETTLE", "0.3"))
time.sleep(settle)
ser.reset_input_buffer()

for cmd in sys.argv[1:]:
    print(f"\n===== $ {cmd} =====", flush=True)
    ser.write((cmd + "\n").encode())
    ser.flush()
    buf = bytearray()
    deadline = time.time() + PER_CMD_TIMEOUT
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if EOT in buf:
                break
    text = buf.split(bytes([EOT]))[0].decode("utf-8", errors="replace")
    if text.endswith("# "):
        text = text[:-2]
    print(text, flush=True)
    if EOT not in buf:
        print(f"[no EOT within {PER_CMD_TIMEOUT:.0f}s - reply may be incomplete]", flush=True)

ser.close()
