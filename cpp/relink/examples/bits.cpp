// Bits -- 8 on/off switches on one UInt8 pin.
//
// Dashboard setup:  V5 UInt8 (0–255), Switch widgets on V5 with Bit 0–7
// Pairs with:       the Arduino "Bits" example (8 relay channels on bits 0–7)
// Run:              ./relink_bits
// You'll see:       "V5 0b10000101 (133)" when anything changes the pin, and bit 4
//                   blinking on the dashboard. relink::Bits keeps the one pin value:
//                   read it as a number (bits.u8) or as bits (bits.asBit(2)).
#include <relink/relink.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Keep V5's whole value, so bits set elsewhere stay when we send ours.
    //    The library stores no bits; this program owns the value.
    std::atomic<int32_t> v5{0};
    r.onPin(5, [&](const relink::Value& v) {
        relink::Bits b = v;
        v5 = b.i32;
        printf("V5 0b");
        for (int i = 7; i >= 0; i--) printf("%d", b.asBit(i));
        printf(" (%u)\n", b.u8);
        fflush(stdout);
    });

    r.start();

    // 4. Change one bit, send the whole value (UInt8 datastream -> 1 byte)
    for (bool on = true;; on = !on) {
        relink::Bits b(v5.load());
        b.setBit(4, on);
        v5 = b.i32;
        r.virtualWrite(5, b);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
