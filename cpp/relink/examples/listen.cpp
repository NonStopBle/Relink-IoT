// Listen -- print every pin write from the dashboard or other programs.
//
// Dashboard setup:  any datastreams
// Run:              ./relink_listen [seconds]
// You'll see:       "V3 = 42" for every new value, then writes/s per pin and
//                   how you're linked (p2p / relay) every 5 s.
#include <relink/relink.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include "settings.hpp"   // 1. Your settings live here

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 1 << 30;

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);

    // 3. One handler for every pin
    std::mutex m;
    std::map<int, std::string> last;
    std::map<int, long> counts;
    r.onAny([&](uint8_t pin, const relink::Value& v) {
        std::lock_guard<std::mutex> g(m);
        counts[pin]++;
        std::string s = v.asString();
        if (last[pin] != s) { last[pin] = s; printf("V%u = %s\n", pin, s.c_str()); }
    });

    r.start();

    // 4. Rates + status every 5 s
    for (int t = 0; t < secs; t += 5) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        {
            std::lock_guard<std::mutex> g(m);
            printf("[listen]");
            if (counts.empty()) printf(" nothing yet");
            for (auto& [pin, n] : counts) printf(" V%d %.1f/s", pin, n / 5.0);
            printf("\n");
            counts.clear();
        }
        printStatus(r);
    }
    r.stop();
}
