"""Camera -- watch a device's live camera, peer-to-peer.

Dashboard setup:  V10 Camera, V11 ByteArray
Pairs with:       the Arduino "CameraStream" example
Needs:            pip install opencv-python numpy
Run:              python3 camera.py                       # opens a window, q quits
                  python3 camera.py --headless            # only prints fps
                  python3 camera.py --set "fs=QVGA;q=20"  # change camera settings first
You'll see:       the video, plus "15.0 fps  12.3 KB  decode 2.1 ms" every 2 s.
"""
import sys, time
import numpy as np
import cv2
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""              # camera needs encryption (P2P mesh)
SERVER = "relink-iot.com"
STREAM_PIN, SETTINGS_PIN = 10, 11

headless = "--headless" in sys.argv
settings = sys.argv[sys.argv.index("--set") + 1] if "--set" in sys.argv else None

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Each frame arrives as JPEG bytes ──
latest = None
stats = {"n": 0, "bytes": 0, "dec": 0.0}

@r.on_camera(STREAM_PIN)
def on_frame(jpeg, frame_id):
    global latest
    t = time.perf_counter()
    img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    stats["dec"] += time.perf_counter() - t
    stats["n"] += 1
    stats["bytes"] += len(jpeg)
    if img is not None:
        latest = img

r.start()
if settings:
    r.virtual_write(SETTINGS_PIN, settings)

# ── 4. Show it ──
last = time.time()
while True:
    if not headless and latest is not None:
        cv2.imshow("Relink camera", latest)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break
    else:
        time.sleep(0.01)
    if time.time() - last >= 2:
        dt, n = time.time() - last, stats["n"]
        print(f"{n / dt:5.1f} fps  {stats['bytes'] / max(n, 1) / 1024:4.1f} KB  "
              f"decode {stats['dec'] / max(n, 1) * 1000:.1f} ms  peers {r.peers()}", flush=True)
        stats.update(n=0, bytes=0, dec=0.0)
        last = time.time()
r.stop()
