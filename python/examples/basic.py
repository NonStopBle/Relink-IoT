"""Basic -- send a value to V0, react when V1 changes.

Dashboard setup:  V0 Float  -> Label or Gauge widget
                  V1 Integer -> Toggle or Button widget
Run:              python3 basic.py
You'll see:       V0 moving on the dashboard; flip the Toggle and "V1 = 1" prints here.
"""
import math, time
from relink_iot import Relink

# ── 1. Your settings (or set RELINK_TOKEN / RELINK_KEY / RELINK_SERVER) ──
TOKEN  = "your-device-token"
KEY    = ""              # "" = no encryption
SERVER = "relink-iot.com"

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. React to writes from the dashboard or another program ──
@r.on_pin(1)
def on_v1(value):
    print("V1 =", value)

r.start()   # runs in a background thread

# ── 4. Send values ──
while True:
    r.virtual_write(0, round(20 + 5 * math.sin(time.time()), 2))
    time.sleep(0.1)
