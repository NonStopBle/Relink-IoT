"""Listen -- print every pin write from the dashboard or other programs.

Dashboard setup:  any datastreams
Run:              python3 listen.py
You'll see:       "V3 = 42" for every new value, and how you're linked (p2p / relay) every 5 s.
"""
import os, time
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = os.environ.get("RELINK_TOKEN",  "your-device-token")
KEY    = os.environ.get("RELINK_KEY",    "")
SERVER = os.environ.get("RELINK_SERVER", "192.168.96.50")

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. One handler for every pin ──
last = {}

@r.on_any
def on_write(pin, value):
    if last.get(pin) != value:          # only print changes, not repeats
        last[pin] = value
        print(f"V{pin} = {value!r}")

r.start()

# ── 4. Status every 5 s ──
while True:
    time.sleep(5)
    print("connected" if r.connected else "offline", "| peers:", r.peers() or "none")
