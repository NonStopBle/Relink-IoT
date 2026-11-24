// Switch -- turn something on and off from a dashboard Switch.
//
// Dashboard setup:  V1 Integer -> Switch widget
// Pairs with:       the Arduino "Turn an LED on and off" example
// Run:              ./relink_switch
// You'll see:       "LED on" / "LED off" here every time you flip the Switch.
//                   Replace the printf with your own code (a GPIO pin, a script, a command).
#include <relink/relink.hpp>
#include <chrono>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Runs every time the Switch changes
    r.onPin(1, [](const relink::Value& v) {
        printf("%s\n", v.asInt() ? "LED on" : "LED off");   // 1 = on, 0 = off
        fflush(stdout);
    });

    r.start();   // runs in a background thread

    // 4. Keep the program alive
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
