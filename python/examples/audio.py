"""Audio -- listen to a device's mic and talk back, peer-to-peer.

Dashboard setup:  V0 Audio
Pairs with:       the Arduino "AudioStream" example (XIAO ESP32S3 Sense mic)
Needs:            nothing extra (WAV files only; play them with any player)
Run:              python3 audio.py                     # record 10 s of the device's mic to heard.wav
                  python3 audio.py --seconds 30        # longer
                  python3 audio.py --tone              # also send a 440 Hz tone to the device
                  python3 audio.py --play hello.wav    # also send a WAV file (8 kHz mono 16-bit)
You'll see:       "50 packets/s  level 52 dB  peers 1" every second, then heard.wav.
                  With --tone the device prints "[talk] level 77 dB, 50 packets/s".
"""
import math, os, sys, time, wave
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = os.environ.get("RELINK_TOKEN", "your-device-token")
KEY    = os.environ.get("RELINK_KEY", "")         # audio needs encryption (P2P mesh)
SERVER = os.environ.get("RELINK_SERVER", "relink-iot.com")
PIN    = 0

arg = lambda name, default=None: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
seconds = float(arg("--seconds", 10))
play = arg("--play")

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Each 20 ms of the device's mic arrives as 160 samples ──
heard = bytearray()
stats = {"n": 0, "sq": 0.0, "count": 0}

@r.on_audio(PIN)
def on_sound(samples):
    heard.extend(b"".join(s.to_bytes(2, "little", signed=True) for s in samples))
    stats["n"] += 1
    stats["sq"] += sum(s * s for s in samples)
    stats["count"] += len(samples)

r.start()

# What to say: a tone, a WAV file, or nothing.
talk = None
if play:
    with wave.open(play) as w:
        assert w.getframerate() == 8000 and w.getnchannels() == 1 and w.getsampwidth() == 2, "need 8 kHz mono 16-bit"
        talk = w.readframes(w.getnframes())
elif "--tone" in sys.argv:
    talk = b"".join(int(9000 * math.sin(2 * math.pi * 440 * i / 8000)).to_bytes(2, "little", signed=True)
                    for i in range(int(8000 * seconds)))

# ── 4. Run: send 20 ms every 20 ms, print once a second ──
start = last = time.time()
sent = 0
while time.time() - start < seconds:
    if talk and sent * 320 < len(talk) and r.peers():
        r.audio_write(PIN, talk[sent * 320:(sent + 1) * 320])
        sent += 1
    time.sleep(0.02)
    if time.time() - last >= 1:
        n, c = stats["n"], stats["count"]
        level = 20 * math.log10(math.sqrt(stats["sq"] / c) + 1) if c else 0
        print(f"{n / (time.time() - last):4.0f} packets/s  level {level:3.0f} dB  peers {r.peers()}"
              + (f"  sent {sent}" if talk else ""), flush=True)
        stats.update(n=0, sq=0.0, count=0)
        last = time.time()

with wave.open("heard.wav", "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000); w.writeframes(bytes(heard))
print(f"heard.wav: {len(heard) / 16000:.1f} s")
