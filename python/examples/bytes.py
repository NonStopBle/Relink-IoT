"""ByteArray -- receive a packed record on V2, send raw bytes to V3.

Dashboard setup:  V2 ByteArray, V3 ByteArray
Pairs with:       the Arduino "ByteArray" example (it fills V2)
Run:              python3 bytes.py
You'll see:       "V2 accel=(841, 540, 981) online=1" as the ESP32 sends.
"""
import os, struct, time
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = os.environ.get("RELINK_TOKEN",  "your-device-token")
KEY    = os.environ.get("RELINK_KEY",    "")
SERVER = os.environ.get("RELINK_SERVER", "192.168.96.50")

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Unpack V2: [ax int16][ay int16][az int16][status uint8], little-endian ──
@r.on_pin(2)
def on_v2(value):
    if isinstance(value, bytes) and len(value) >= 7:
        ax, ay, az, status = struct.unpack_from("<hhhB", value)
        print(f"V2 accel=({ax}, {ay}, {az}) online={status}")

r.start()

# ── 4. Send raw bytes: a Python `bytes` value goes out as ByteArray ──
n = 0
while True:
    r.virtual_write(3, bytes([0xCA, 0xFE, n & 0xFF]))
    n += 1
    time.sleep(1)
