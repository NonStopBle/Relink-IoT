"""Camera send -- stream a webcam as live video on a Camera pin, peer-to-peer,
the same way an ESP32-CAM does: to the dashboard's Live Camera widget, or to
another program watching with on_camera().

Dashboard setup:  V10 Camera, V11 ByteArray (settings, optional)
Pairs with:       a Live Camera widget on V10, or the Python / C++ "camera" examples
Needs:            pip install opencv-python numpy; an encryption key (video goes
                  peer-to-peer, never through the relay); for the dashboard widget,
                  librelink_rtc.so (python/native/build.sh, needs libmbedtls-dev)
Run:              python3 camera_send.py                 # first webcam
                  python3 camera_send.py --cam 2         # another camera
                  python3 camera_send.py --video clip.mp4 # a video file, looped
                  python3 camera_send.py --pattern       # no camera: moving test image
                  python3 camera_send.py --size 640x480 --fps 15 --quality 70
                  python3 camera_send.py --pin 12        # another Camera pin
You'll see:       "watching: yes  15.0 fps  24.1 KB/frame" every 2 s while a viewer
                  watches; nothing is captured or sent while nobody does.
Note:             one viewer at a time: whoever starts watching last takes the stream.
"""
import sys, time
import numpy as np
import cv2
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = "your-64-hex-char-key"   # required: video is P2P over the encrypted mesh
SERVER = "relink-iot.com"

def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

STREAM_PIN = int(arg("--pin", 10))

cam_index = int(arg("--cam", 0))
width, height = (int(x) for x in arg("--size", "640x480").split("x"))
max_fps = float(arg("--fps", 15))
quality = int(arg("--quality", 70))
pattern = "--pattern" in sys.argv
video = arg("--video", None)

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)
if not r.enable_p2p():   # the dashboard's widget; mesh viewers work without it
    print("dashboard P2P off (no key or no librelink_rtc.so): mesh viewers only")
r.start()

# ── 3. Frame source: a webcam, or a moving test image ──
cap = None
if video:
    cap = cv2.VideoCapture(video)
    if not cap.isOpened():
        sys.exit(f"can't open {video}")
elif not pattern:
    cap = cv2.VideoCapture(cam_index)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    if not cap.isOpened():
        sys.exit(f"camera {cam_index} not found -- try --cam N or --pattern")

def grab():
    if cap is not None:
        ok, img = cap.read()
        if not ok and video:                       # end of the file: loop
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            ok, img = cap.read()
        if ok and img.shape[1] != width:          # scale to --size width, keep aspect
            img = cv2.resize(img, (width, img.shape[0] * width // img.shape[1]), interpolation=cv2.INTER_AREA)
        return img if ok else None
    img = np.zeros((height, width, 3), np.uint8)
    x = int((time.time() * 120) % width)
    cv2.rectangle(img, (x, height // 3), (x + 80, height // 3 + 80), (80, 200, 120), -1)
    cv2.putText(img, time.strftime("%H:%M:%S"), (20, height - 30),
                cv2.FONT_HERSHEY_SIMPLEX, 1.2, (255, 255, 255), 2)
    return img

# ── 4. Loop: only capture and encode while someone watches ──
sent, size, last_print = 0, 0, time.monotonic()
while True:
    t0 = time.monotonic()
    if r.virtual_image_wanted():
        img = grab()
        if img is not None:
            ok, jpeg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, quality])
            if ok and r.virtual_image_write(STREAM_PIN, jpeg.tobytes()):
                sent += 1
                size += len(jpeg)
    else:
        time.sleep(0.1)   # nobody watching: idle

    if time.monotonic() - last_print >= 2:
        dt = time.monotonic() - last_print
        print(f"watching: {'yes' if r.virtual_image_wanted() else 'no '}  "
              f"{sent / dt:.1f} fps  {size / max(sent, 1) / 1024:.1f} KB/frame", flush=True)
        sent, size, last_print = 0, 0, time.monotonic()

    wait = 1 / max_fps - (time.monotonic() - t0)
    if wait > 0:
        time.sleep(wait)
