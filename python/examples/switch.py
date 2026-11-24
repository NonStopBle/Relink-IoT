"""Switch -- turn something on and off from a dashboard Switch.

Dashboard setup:  V1 Integer -> Switch widget
Pairs with:       the Arduino "Turn an LED on and off" example
Run:              python3 switch.py
You'll see:       "LED on" / "LED off" here every time you flip the Switch.
                  Replace the print() with your own code (a GPIO pin, a script, a command).
"""
import time
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""                       # encryption key: needed for P2P
SERVER = "relink-iot.com"

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Runs every time the Switch changes ──
@r.on_pin(1)
def on_switch(value):
    on = int(value)               # 1 = on, 0 = off
    print("LED on" if on else "LED off")

r.start()   # runs in a background thread

# ── 4. Keep the program alive ──
while True:
    time.sleep(1)
