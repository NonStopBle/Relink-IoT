// ByteArray -- receive a packed record on V2, send raw bytes to V3.
//
// Dashboard setup:  V2 ByteArray, V3 ByteArray
// Pairs with:       the Arduino "ByteArray" example (it fills V2)
// Run:              ./relink_bytes
// You'll see:       "V2 accel=(841, 540, 981) online=1" as the ESP32 sends.
#include <relink/relink.hpp>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Unpack V2: [ax int16][ay int16][az int16][status uint8], little-endian.
    //    Raw bytes are in Value::s.
    r.onPin(2, [](const relink::Value& v) {
        if (v.type != relink::DType::Bytes || v.s.size() < 7) return;
        int16_t a[3]; memcpy(a, v.s.data(), 6);
        printf("V2 accel=(%d, %d, %d) online=%u\n", a[0], a[1], a[2], (uint8_t)v.s[6]);
    });

    r.start();

    // 4. Send raw bytes: a std::vector<uint8_t> goes out as ByteArray
    for (uint8_t n = 0;; n++) {
        r.virtualWrite(3, std::vector<uint8_t>{0xCA, 0xFE, n});
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
