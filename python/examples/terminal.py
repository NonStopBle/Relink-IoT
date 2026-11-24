"""Terminal -- type commands on the dashboard and get answers from your program.

Dashboard setup:  V1 String -> Serial Terminal widget
Pairs with:       the Arduino "Send commands from a terminal" example
Run:              python3 terminal.py
You'll see:       type "status" in the Terminal widget and read the answer there.
"""
import time
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""
SERVER = "relink-iot.com"

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)
started = time.time()
led = False

# ── 3. Runs when text arrives on V1 ──
@r.on_pin(1)
def on_command(value):
    global led
    cmd = str(value).strip().lower()
    if cmd == "led on":
        led = True;  reply = "LED is on"
    elif cmd == "led off":
        led = False; reply = "LED is off"
    elif cmd == "status":
        reply = f"Up {int(time.time() - started)} s, LED {'on' if led else 'off'}"
    else:
        reply = "Commands: led on, led off, status"
    if reply != value:             # don't answer our own reply
        r.virtual_write(1, reply)  # up to 58 characters

r.start()   # runs in a background thread

# ── 4. Keep the program alive ──
while True:
    time.sleep(1)
