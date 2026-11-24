"""Sensor -- send a reading to the dashboard once a second.

Dashboard setup:  V0 Float -> Gauge or Value Display widget
Pairs with:       the Arduino "Temperature and humidity" example
Run:              python3 sensor.py
You'll see:       the computer's CPU temperature on the dashboard. Linux only: it reads
                  /sys/class/thermal. Swap read_sensor() for your own sensor code.
"""
import time
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""
SERVER = "relink-iot.com"

def read_sensor():
    with open("/sys/class/thermal/thermal_zone0/temp") as f:
        return int(f.read()) / 1000.0         # degrees C

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)
r.start()   # runs in a background thread

# ── 4. Send once a second ──
while True:
    try:
        t = read_sensor()
        r.virtual_write(0, t)
        print(f"V0 = {t:.1f}")
    except OSError:
        print("No CPU temperature on this computer: replace read_sensor()")
    time.sleep(1)
