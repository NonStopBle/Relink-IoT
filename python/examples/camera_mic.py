"""Camera + mic -- live video and two-way sound between programs, peer-to-peer.

One program, two roles on the same token:
  viewer (default)  shows the camera, plays the sound on the PC speakers and
                    sends the PC microphone back (talk)
  --send            acts like the camera board: streams the webcam and the PC
                    microphone, and plays what the viewer says

Dashboard setup:  V10 Camera, V11 ByteArray, V0 Audio
Pairs with:       itself (--send on one PC, viewer on another), or the Arduino
                  "CameraStreamWithMic" example (run the viewer)
Needs:            pip install opencv-python numpy sounddevice; an encryption key
                  (video and sound go peer-to-peer, never through the relay)
Run:              python3 camera_mic.py                  # viewer: window + speaker + mic, q quits
                  python3 camera_mic.py --send           # device: webcam + mic
                  python3 camera_mic.py --send --pattern # no webcam: moving test image
                  python3 camera_mic.py --mute           # listen only, don't send the mic
                  python3 camera_mic.py --tone 440       # send a 440 Hz tone instead of the mic
                  python3 camera_mic.py --headless --save heard.wav --seconds 20
You'll see:       "video 15.0 fps | heard 50 pkt/s 52 dB | said 50 pkt/s | peers 1"
                  every 2 s. Use headphones: a speaker next to the mic echoes.
"""
import collections, math, os, sys, time
import numpy as np
import cv2
import sounddevice as sd
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = os.environ.get("RELINK_TOKEN", "your-device-token")
KEY    = os.environ.get("RELINK_KEY", "")     # required: P2P over the encrypted mesh
SERVER = os.environ.get("RELINK_SERVER", "relink-iot.com")
STREAM_PIN, SETTINGS_PIN, AUDIO_PIN = 10, 11, 0
RATE, PKT = 8000, 160             # 8 kHz mono, 20 ms per packet

arg = lambda name, default=None: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
send     = "--send" in sys.argv
headless = "--headless" in sys.argv or send
mute     = "--mute" in sys.argv
tone     = float(arg("--tone", 0))
seconds  = float(arg("--seconds", 0))      # 0 = until q / Ctrl+C
save     = arg("--save")

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)
if send:
    r.enable_p2p()        # lets the dashboard widgets watch too (needs librelink_rtc.so)

# ── 3. Handlers ──
st = collections.Counter()
latest = None             # newest decoded frame (viewer)
heard_wav = bytearray()
play = collections.deque(maxlen=25)   # packets waiting for the speaker, max 0.5 s

if not send:
    @r.on_camera(STREAM_PIN)
    def on_frame(jpeg, frame_id):
        global latest
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is not None:
            latest = img
            st["frames"] += 1

@r.on_audio(AUDIO_PIN)
def on_sound(samples):
    a = np.asarray(samples, np.int16)
    play.append(a)
    st["heard"] += 1
    st["sq"] += float(np.dot(a.astype(np.float64), a))
    if save:
        heard_wav.extend(a.tobytes())

# Speaker: the sound card asks for 20 ms at a time. Wait until 3 packets are
# queued before starting (absorbs Wi-Fi jitter); a gap plays as silence.
primed = False
def speaker(out, frames, t, status):
    global primed
    if not primed and len(play) >= 3:
        primed = True
    if primed and play:
        out[:, 0] = play.popleft()
    else:
        out.fill(0)
        primed = primed and bool(play)

# Mic: the sound card hands over 20 ms blocks; they're sent from the main loop.
mic = collections.deque(maxlen=25)
def microphone(indata, frames, t, status):
    mic.append(indata[:, 0].copy())

direct = lambda: "p2p" in r.peers().values()   # sound only goes peer-to-peer
r.start()
streams = [sd.OutputStream(RATE, PKT, channels=1, dtype="int16", callback=speaker)]
if not mute and not tone:
    streams.append(sd.InputStream(RATE, PKT, channels=1, dtype="int16", callback=microphone))
for s in streams:
    s.start()

# Webcam / test image for --send
cap = None
if send and "--pattern" not in sys.argv:
    cap = cv2.VideoCapture(int(arg("--cam", 0)))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    if not cap.isOpened():
        sys.exit("no webcam -- try --cam N or --pattern")

def grab():
    if cap is not None:
        ok, img = cap.read()
        return img if ok else None
    img = np.zeros((480, 640, 3), np.uint8)
    x = int((time.time() * 120) % 640)
    cv2.rectangle(img, (x, 160), (x + 80, 240), (80, 200, 120), -1)
    cv2.putText(img, time.strftime("%H:%M:%S"), (20, 450), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (255, 255, 255), 2)
    return img

# ── 4. Loop ──
start = last = last_frame = time.monotonic()
phase = 0
try:
    while not seconds or time.monotonic() - start < seconds:
        now = time.monotonic()
        # talk: the mic blocks, or a tone paced at one packet per 20 ms
        if tone and not mute and direct():
            while phase < (now - start) * RATE:
                n = np.arange(phase, phase + PKT)
                st["said"] += r.audio_write(AUDIO_PIN, (9000 * np.sin(2 * math.pi * tone * n / RATE)).astype(np.int16).tobytes())
                phase += PKT
        while mic:
            block = mic.popleft()
            if direct():
                st["said"] += r.audio_write(AUDIO_PIN, block.tobytes())

        # video: --send streams 15 fps while someone watches; the viewer shows it
        if send and r.virtual_image_wanted() and now - last_frame >= 1 / 15:
            last_frame = now
            img = grab()
            if img is not None:
                ok, jpeg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 70])
                if ok and r.virtual_image_write(STREAM_PIN, jpeg.tobytes()):
                    st["frames"] += 1
        if not headless and latest is not None:
            cv2.imshow("Relink camera", latest)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
        else:
            time.sleep(0.005)

        if now - last >= 2:
            dt, h = now - last, st["heard"]
            level = 20 * math.log10(math.sqrt(st["sq"] / (h * PKT)) + 1) if h else 0
            print(f"video {st['frames'] / dt:4.1f} fps | heard {h / dt:3.0f} pkt/s {level:3.0f} dB | "
                  f"said {st['said'] / dt:3.0f} pkt/s | peers {r.peers() or 'none'}", flush=True)
            st.clear()
            last = now
except KeyboardInterrupt:
    pass

for s in streams:
    s.stop()
r.stop()
if save:
    import wave
    with wave.open(save, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(bytes(heard_wav))
    print(f"{save}: {len(heard_wav) / 2 / RATE:.1f} s")
