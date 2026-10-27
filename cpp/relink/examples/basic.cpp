// Basic -- send a value to V0, react when V1 changes.
//
// Dashboard setup:  V0 Float  -> Label or Gauge widget
//                   V1 Integer -> Toggle or Button widget
// Run:              ./relink_basic
// You'll see:       V0 moving on the dashboard; flip the Toggle and "V1 = 1" prints here.
#include <relink/relink.hpp>
#include <chrono>
#include <cmath>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. React to writes from the dashboard or another program
    r.onPin(1, [](const relink::Value& v) { printf("V1 = %s\n", v.asString().c_str()); });

    r.start();   // runs in a background thread

    // 4. Send values
    for (int i = 0;; i++) {
        r.virtualWrite(0, 20.0f + 5.0f * std::sin(i / 10.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (i % 50 == 0) printStatus(r);
    }
}
