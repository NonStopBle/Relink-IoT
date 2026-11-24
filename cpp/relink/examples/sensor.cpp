// Sensor -- send a reading to the dashboard once a second.
//
// Dashboard setup:  V0 Float -> Gauge or Value Display widget
// Pairs with:       the Arduino "Temperature and humidity" example
// Run:              ./relink_sensor
// You'll see:       the computer's CPU temperature on the dashboard. Linux only: it reads
//                   /sys/class/thermal. Swap readSensor() for your own sensor code.
#include <relink/relink.hpp>
#include <chrono>
#include <fstream>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

// Returns degrees C, or -1000 when there is no sensor
static float readSensor() {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    long milli;
    return (f >> milli) ? milli / 1000.0f : -1000.0f;
}

int main() {
    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    r.start();   // runs in a background thread

    // 3. Send once a second
    for (;;) {
        float t = readSensor();
        if (t > -999) { r.virtualWrite(0, t); printf("V0 = %.1f\n", t); }
        else printf("No CPU temperature on this computer: replace readSensor()\n");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
