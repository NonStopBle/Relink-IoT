// Terminal -- type commands on the dashboard and get answers from your program.
//
// Dashboard setup:  V1 String -> Serial Terminal widget
// Pairs with:       the Arduino "Send commands from a terminal" example
// Run:              ./relink_terminal
// You'll see:       type "status" in the Terminal widget and read the answer there.
#include <relink/relink.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

int main() {
    auto started = std::chrono::steady_clock::now();
    bool led = false;

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. Runs when text arrives on V1
    r.onPin(1, [&](const relink::Value& v) {
        std::string cmd = v.asString();
        cmd.erase(0, cmd.find_first_not_of(" \t\r\n"));
        cmd.erase(cmd.find_last_not_of(" \t\r\n") + 1);
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return std::tolower(c); });

        std::string reply;
        if (cmd == "led on")       { led = true;  reply = "LED is on"; }
        else if (cmd == "led off") { led = false; reply = "LED is off"; }
        else if (cmd == "status") {
            long s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
            reply = "Up " + std::to_string(s) + " s, LED " + (led ? "on" : "off");
        }
        else reply = "Commands: led on, led off, status";
        if (reply != v.asString()) r.virtualWrite(1, reply);   // up to 58 characters
    });

    r.start();   // runs in a background thread

    // 4. Keep the program alive
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
