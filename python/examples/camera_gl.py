"""Camera (OpenGL) -- watch a device's live camera in an OpenGL window, peer-to-peer.

Dashboard setup:  V10 Camera, V11 ByteArray
Pairs with:       the Arduino "CameraStream" example
Needs:            pip install glfw PyOpenGL opencv-python numpy
Run:              python3 camera_gl.py            # Esc or q quits
                  python3 camera_gl.py 20         # quit after 20 s (prints stats)
You'll see:       the video drawn as a GL texture, and "15.0 fps ..." in the title and terminal.
"""
import sys, threading, time
import cv2
import glfw
import numpy as np
from OpenGL import GL
from relink_iot import Relink

# ── 1. Your settings ──
TOKEN  = "your-device-token"
KEY    = ""              # camera needs encryption (P2P mesh)
SERVER = "relink-iot.com"
STREAM_PIN = 10
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else float("inf")

# ── 2. Connect ──
r = Relink(TOKEN, KEY, SERVER)

# ── 3. Decode each JPEG on the receive thread; the GL thread only uploads ──
lock = threading.Lock()
latest = None                      # newest RGB frame not yet uploaded
stats = {"n": 0, "bytes": 0}

@r.on_camera(STREAM_PIN)
def on_frame(jpeg, frame_id):
    global latest
    img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return
    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    with lock:
        latest = rgb
        stats["n"] += 1
        stats["bytes"] += len(jpeg)

r.start()

# ── 4. OpenGL window: one texture, redrawn when a new frame lands ──
if not glfw.init():
    sys.exit("glfw init failed")
win = glfw.create_window(640, 480, "Relink camera (OpenGL)", None, None)
glfw.make_context_current(win)
glfw.swap_interval(1)
glfw.set_key_callback(win, lambda w, key, *_: key in (glfw.KEY_ESCAPE, glfw.KEY_Q) and glfw.set_window_should_close(w, True))

tex = GL.glGenTextures(1)
GL.glBindTexture(GL.GL_TEXTURE_2D, tex)
GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)
GL.glEnable(GL.GL_TEXTURE_2D)
size = None                        # (w, h) of the allocated texture

start = last = time.time()
while not glfw.window_should_close(win) and time.time() - start < SECONDS:
    with lock:
        frame, latest = latest, None
    if frame is not None:
        h, w = frame.shape[:2]
        if size != (w, h):         # allocate once per resolution, then update in place
            GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGB, w, h, 0, GL.GL_RGB, GL.GL_UNSIGNED_BYTE, frame)
            size = (w, h)
        else:
            GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0, w, h, GL.GL_RGB, GL.GL_UNSIGNED_BYTE, frame)

    fw, fh = glfw.get_framebuffer_size(win)
    GL.glViewport(0, 0, fw, fh)
    GL.glClearColor(0.05, 0.05, 0.06, 1)
    GL.glClear(GL.GL_COLOR_BUFFER_BIT)
    if size:
        # Letterbox: keep the camera's aspect ratio inside the window
        sx = min(1.0, (fh * size[0] / size[1]) / fw)
        sy = min(1.0, (fw * size[1] / size[0]) / fh)
        GL.glBegin(GL.GL_QUADS)
        GL.glTexCoord2f(0, 1); GL.glVertex2f(-sx, -sy)
        GL.glTexCoord2f(1, 1); GL.glVertex2f( sx, -sy)
        GL.glTexCoord2f(1, 0); GL.glVertex2f( sx,  sy)
        GL.glTexCoord2f(0, 0); GL.glVertex2f(-sx,  sy)
        GL.glEnd()
    glfw.swap_buffers(win)
    glfw.poll_events()

    if time.time() - last >= 2:
        with lock:
            n, b = stats["n"], stats["bytes"]
            stats.update(n=0, bytes=0)
        dt = time.time() - last
        msg = f"{n / dt:5.1f} fps  {b / max(n, 1) / 1024:4.1f} KB  {size[0]}x{size[1]}" if size else "waiting for frames"
        glfw.set_window_title(win, f"Relink camera (OpenGL) -- {msg}")
        print(f"[gl-py] {msg}  peers {r.peers()}", flush=True)
        last = time.time()

glfw.terminate()
r.stop()
