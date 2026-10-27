#ifndef WEBRTC_LITE_CONFIG_H
#define WEBRTC_LITE_CONFIG_H
/*
 * Single on/off switch for the whole WebRTC-lite stack (stun_lite,
 * dtls_lite, dtls_defrag, sctp_lite, dcep_lite, rtc_session), matching
 * RelinkIoT.h's existing RELINK_NO_OTA / RELINK_NO_ENCRYPTION convention.
 *
 * Every .c file in this module #includes this header FIRST and wraps its
 * whole body in `#ifdef RELINK_HAS_WEBRTC`, so when disabled the stack
 * compiles to nothing: zero flash, zero RAM, no dangling symbols (callers
 * gate their own use behind the same check, e.g. see demo main.cpp).
 *
 * PlatformIO users: pass -DRELINK_NO_WEBRTC=1 via build_flags in
 * platformio.ini. That defines the macro on the compiler's command line
 * for every file PlatformIO compiles, library included, so the #ifndef
 * below picks it up with no source edit needed.
 *
 * Arduino IDE users: the IDE has no equivalent to build_flags, and even if
 * it did, a #define written at the top of your .ino would NOT reach this
 * library's .c files -- the IDE compiles each source file as its own
 * translation unit, and a sketch-level #define only affects the sketch's
 * own .cpp. The only setting that's guaranteed to reach every file
 * uniformly is one written in a header every file includes first -- this
 * one. So: uncomment the line below directly in this file to disable the
 * stack. (Same instruction applies to any other RELINK_NO_* flag you see
 * mentioned in this library's headers -- none of them work from inside an
 * Arduino IDE sketch; all of them must be edited here or in RelinkIoT.h
 * directly for that IDE.)
 */
// #define RELINK_NO_WEBRTC

#ifndef RELINK_NO_WEBRTC
#define RELINK_HAS_WEBRTC 1
#endif
#endif
