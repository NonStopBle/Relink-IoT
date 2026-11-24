// Button -- send a button press to the dashboard.
//
// Dashboard setup:  V0 Integer -> LED Indicator widget
// Pairs with:       the Arduino "Show a button press" example
// Run:              ./relink_button
// You'll see:       press Enter once and the LED on the dashboard lights ("pressed");
//                   press Enter again and it goes off ("released").
#include <relink/relink.hpp>
#include <iostream>
#include <string>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    r.start();   // runs in a background thread

    // 3. Each Enter flips the state and sends it
    int pressed = 0;
    std::string line;
    for (;;) {
        printf("Press Enter to flip the button... "); fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        pressed = 1 - pressed;
        r.virtualWrite(0, (int32_t)pressed);
        printf("%s\n", pressed ? "pressed" : "released");
    }
    r.stop();
}
