"""Bits -- 8 on/off switches on one UInt8 pin.

Dashboard setup:  V5 UInt8 (0–255), Switch widgets on V5 with Bit 0–7
Pairs with:       the Arduino "Bits" example (8 relay channels on bits 0–7)
Run:              python3 bits.py
You'll see:       "V5 0b10000101 (133)" when anything changes the pin, and bit 4
                  blinking on the dashboard. Bits keeps the one pin value: it is a
                  plain int (int(relays)) that you can also read as bits (relays.as_bit(2)).
"""
import time
from relink_iot import Relink, Bits

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""
SERVER = "relink-iot.com"

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Keep V5's whole value, so bits set elsewhere stay when we send ours ──
relays = Bits(0)

@r.on_pin(5)
def on_v5(value):
    global relays
    relays = Bits(int(value))
    print(f"V5 0b{int(relays) & 0xFF:08b} ({int(relays)})")

r.start()

# ── 4. Change one bit, send the whole value (UInt8 datastream -> 1 byte) ──
on = True
while True:
    relays = relays.set_bit(4, on)
    r.virtual_write(5, relays)
    on = not on
    time.sleep(2)
