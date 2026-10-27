// Shared by the examples: your device settings. Environment variables
// RELINK_TOKEN / RELINK_KEY / RELINK_SERVER win over the values below.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#define MY_TOKEN  "xW70sxgsD5dUwVbUAWwa6CRSUycSlajF"
#define MY_KEY    "0f5a1815f63434deb74a2136c8b938db9008b1fc24e0b88630dd01a78cb92cd"                    // "" = no encryption
#define MY_SERVER "192.168.10.165"

inline const char* setting(const char* env, const char* fallback) {
    const char* v = getenv(env);
    return v && *v ? v : fallback;
}
#define RELINK_TOKEN_  setting("RELINK_TOKEN",  MY_TOKEN)
#define RELINK_KEY_    setting("RELINK_KEY",    MY_KEY)
#define RELINK_SERVER_ setting("RELINK_SERVER", MY_SERVER)

// "connected | peers: 1a2b=p2p 3c4d=relay"
template <class Client>
inline void printStatus(Client& r) {
    printf("%s | peers:", r.connected() ? "connected" : "offline");
    auto peers = r.peers();
    if (peers.empty()) printf(" none");
    for (auto& [id, direct] : peers) printf(" %04x=%s", id, direct ? "p2p" : "relay");
    printf("\n");
    fflush(stdout);
}
