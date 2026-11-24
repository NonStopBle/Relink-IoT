"""Button -- send a button press to the dashboard.

Dashboard setup:  V0 Integer -> LED Indicator widget
Pairs with:       the Arduino "Show a button press" example
Run:              python3 button.py
You'll see:       press Enter once and the LED on the dashboard lights ("pressed");
                  press Enter again and it goes off ("released").
"""
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""
SERVER = "relink-iot.com"

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)
r.start()   # runs in a background thread

# ── 3. Each Enter flips the state and sends it (only when it changes) ──
pressed = 0
while True:
    input("Press Enter to flip the button... ")
    pressed = 1 - pressed
    r.virtual_write(0, pressed)
    print("pressed" if pressed else "released")
