<p align="center">
  <img src="brand/relink-iot-finaly-real.svg" alt="Relink IoT" width="780">
</p>

<h1 align="center">Relink IoT Libraries</h1>

<p align="center">
  Connect devices, applications, and dashboards through one simple IoT platform.
</p>

<p align="center">
  <a href="https://github.com/NonStopBle/Relink-IoT">GitHub</a> ·
  <a href="https://relink-iot.com">Relink IoT</a> ·
  <a href="#quick-start-copy-paste-run">Quick start</a>
</p>

Relink IoT connects ESP32 and ESP8266 devices with Python applications, C++ services,
Raspberry Pi systems, and real-time web dashboards. Monitor sensors, control hardware,
stream camera data, and build connected products using shared virtual pins.

Production client libraries for connecting Arduino devices, C++ applications, and
Python programs to the Relink IoT platform.

The production server is **`relink-iot.com`**. Use this hostname in new projects and
examples. Do not replace it with a local IP address unless you are developing against
a private Relink server.

| Folder | For | Install |
|---|---|---|
| `arduino/RelinkIoT` | ESP32 / ESP8266 firmware (Arduino, PlatformIO) | Arduino IDE: install the ZIP or copy the library; PlatformIO: add the local library and `rweather/Crypto` |
| `cpp` | PCs, Raspberry Pi, servers (Linux/macOS, OpenSSL + zlib) | `cmake -S relink_lib/cpp -B relink_lib/cpp/build && cmake --build relink_lib/cpp/build`; precompiled: `cpp/build_lib.sh` → `cpp/export/relink/` (`find_package(relink)`) |
| `python` | Same, from Python 3 (`from relink_iot import Relink`) | `pip install ./relink_lib/python`; precompiled: `python/build_lib.sh` → `pip install python/export/relink_iot-*.whl` |

Every program that uses the same device token -- the ESP32, a C++ service, a Python
script -- shares one set of virtual pins: a write from any of them reaches the others
and the dashboard.

> **ESP8266 status:** the library and examples build for ESP8266 (NodeMCU v2), but it has
> not been tested on a real board yet. ESP32 is tested on hardware.

## Quick start (copy, paste, run)

**1. On the dashboard:** open your device, then *Datastreams* and add **V0** (Float) and **V1** (Integer).
Add a *Label* widget on V0 and a *Toggle* widget on V1.

**2. Copy the token and key** from the device page.

**3. Run one of these.** Each one sends a number to V0 and prints whatever the dashboard writes to V1.

<details open><summary><b>Python</b></summary>

```bash
pip install ./relink_lib/python
export RELINK_TOKEN=your-token RELINK_KEY=your-key RELINK_SERVER=relink-iot.com
python3 relink_lib/python/examples/basic.py
```
</details>

<details><summary><b>C++ (Linux / macOS / Raspberry Pi)</b></summary>

```bash
cmake -S relink_lib/cpp -B relink_lib/cpp/build && cmake --build relink_lib/cpp/build
export RELINK_TOKEN=your-token RELINK_KEY=your-key RELINK_SERVER=relink-iot.com
relink_lib/cpp/build/relink_basic
```
</details>

<details><summary><b>Arduino (ESP32)</b></summary>

1. Install `arduino/Relink-IoT-1.0.0_Arduino.zip` through *Sketch → Include Library → Add .ZIP Library*.
2. Open *File → Examples → RelinkIoT → BasicSensor*.
3. Fill in Wi-Fi, token, key, and server settings. Keep the server as `relink-iot.com`.
4. Install `rweather/Crypto` if the IDE asks for the dependency, then upload.
</details>

**4. Check it works:** the Label shows the value, and flipping the Toggle prints the new V1 value in the terminal or Serial monitor.

### Pick an example

| I want to… | Arduino | C++ | Python | Datastreams to add |
|---|---|---|---|---|
| Send a value and react to a button | `BasicSensor` | `basic.cpp` | `basic.py` | V0 Float, V1 Integer |
| See everything other programs write | `Listen` | `listen.cpp` | `listen.py` | any |
| Send packed sensor records or raw bytes | `ByteArray` | `bytes.cpp` | `bytes.py` | V2, V3 ByteArray |
| Stream a live camera (P2P) | `CameraStream` | `camera.cpp` | `camera.py` | V10 Camera, V11 ByteArray |
| Show the camera in an OpenGL window | `CameraStream` | `camera_gl.cpp` (freeglut + libjpeg) | `camera_gl.py` (`pip install glfw PyOpenGL`) | V10 Camera, V11 ByteArray |
| Handle WiFi myself | `WiFiEvents` | -- | -- | V0 |
| Let Relink handle WiFi (`Relink.config`) | `RelinkConfig` | -- | -- | V0, V1 |

## Examples (details)

Every library has the same set, so each side of a pair is in the matching folder.

| Example | Arduino (`arduino/RelinkIoT/examples`) | C++ (`cpp/examples`) | Python (`python/examples`) |
|---|---|---|---|
| Write V0, react to V1 | `BasicSensor` | `basic.cpp` | `basic.py` |
| Show everything others write | `Listen` | `listen.cpp` | `listen.py` |
| ByteArray: packed record on V2, raw bytes on V3 | `ByteArray` | `bytes.cpp` | `bytes.py` |
| Live camera, P2P only (V0 Camera, V1 ByteArray settings) | `CameraStream` (sends; needs `RELINK_CAMERA_STREAM`) | `camera.cpp` (receives) | `camera.py` (receives) |
| WiFi handled by the sketch | `WiFiEvents` | -- | -- |
| WiFi handled by Relink (`Relink.config`) | `RelinkConfig` | -- | -- |

Every example starts the same way: a header saying which datastreams to add and what
you'll see, then **1. settings → 2. connect → 3. handlers → 4. loop**. Settings: Python
has them at the top of each file, C++ in `cpp/examples/settings.hpp`, Arduino as
`#define`s at the top of each sketch. In Python and C++, the `RELINK_TOKEN`, `RELINK_KEY`
and `RELINK_SERVER` environment variables override them.

Value types: int, float, text and ByteArray (raw bytes, up to 58 from an ESP32 and 255
from C++/Python) -- matching the datastream types on the dashboard.

## Arduino quick guide

The Arduino examples are complete sketches. Start with `BasicSensor` if this is your
first project, then try `Listen`, `ByteArray`, or `CameraStream`.

### Common Arduino settings

At the top of each sketch, replace only your Wi-Fi credentials and device credentials:

```cpp
#define WIFI_SSID       "YourWiFiName"
#define WIFI_PASS       "YourWiFiPassword"
#define DEVICE_TOKEN    "paste-token-from-device-page"
#define DEVICE_ENC_KEY  "paste-64-hex-character-key"
#define RELINK_SERVER   "relink-iot.com"
```

The encryption key is the 64-character hexadecimal key from the device page. Use an
empty string (`""`) only for a local test without encryption. Never commit real
credentials to GitHub.

### BasicSensor: read a sensor and control an LED

Dashboard setup: `V0` Float, `V1` Integer, and `V2` Float. Add a Label or Gauge on
`V0` and a Slider from 0 to 255 on `V1`.

```cpp
#include <WiFi.h>
#include <RelinkIoT.h>

#define WIFI_SSID       "YourWiFiName"
#define WIFI_PASS       "YourWiFiPassword"
#define DEVICE_TOKEN    "your-device-token"
#define DEVICE_ENC_KEY  "your-64-hex-character-key"
#define RELINK_SERVER   "relink-iot.com"
#define LED_PIN         2

RELINK_WRITE(V1) {
  analogWrite(LED_PIN, constrain(param.asInt(), 0, 255));
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  Relink.config(WIFI_SSID, WIFI_PASS);
  Relink.begin(DEVICE_TOKEN, DEVICE_ENC_KEY, RELINK_SERVER);
}

void loop() {
  Relink.run();
  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    Relink.virtualWrite(V0, analogRead(A0));
  }
}
```

`Relink.run()` must execute continuously so the device can receive dashboard updates.
Avoid long delays in `loop()`.

### Listen: print values received by the device

Use `arduino/examples/Listen/Listen.ino` to see values written by the dashboard, a
Python script, or a C++ service:

```cpp
RELINK_WRITE(V1) {
  Serial.printf("V1 = %d\\n", param.asInt());
}
```

Add matching datastreams for `V0` Float, `V1` Integer, `V2` String, and `V3` ByteArray.

### ByteArray: send compact sensor records

Create `V2` and `V3` as ByteArray datastreams. The example packs several readings into
one message and prints incoming bytes as hexadecimal:

```cpp
uint8_t record[] = { 10, 20, 30, 1 };
Relink.virtualWrite(V2, record, sizeof(record));
```

Open `arduino/examples/ByteArray/ByteArray.ino` for the complete repeating example.

### Wi-Fi options

- **RelinkConfig**: use `Relink.config(WIFI_SSID, WIFI_PASS)` and let Relink connect
  and reconnect Wi-Fi automatically.
- **WiFiEvents**: manage Wi-Fi yourself for static IPs, WiFiManager, or custom logic;
  omit `Relink.config()`.

### CameraStream

On supported ESP32 camera boards, use `arduino/examples/CameraStream/CameraStream.ino`.
Add `V10` as Camera and `V11` as ByteArray, then configure the board pins in
`camera_pins.h`. Camera frames use the direct peer-to-peer path.

## Python

```bash
pip install ./relink_lib/python          # needs `cryptography`
```

```python
from relink_iot import Relink
r = Relink(TOKEN, ENC_KEY, "relink-iot.com")    # ENC_KEY "" = unencrypted

@r.on_pin(1)
def led(v): print("V1 =", v)

r.start()                  # background thread; or call r.run() from your own loop
r.virtual_write(0, 21.5)   # writes made between ticks are batched into one packet
print(r.peers())           # {instance: 'p2p' | 'relay'}
```

## C++ (Linux/macOS, OpenSSL + zlib)

```bash
cmake -S relink_lib/cpp -B relink_lib/cpp/build && cmake --build relink_lib/cpp/build
```

```cpp
#include <relink/relink.hpp>
relink::Client r(TOKEN, ENC_KEY, "relink-iot.com");
r.onPin(1, [](const relink::Value& v) { printf("%d\n", v.asInt()); });
r.start();
r.virtualWrite(0, 21.5f);
```

