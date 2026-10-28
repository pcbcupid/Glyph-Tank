#pragma once
#include <Arduino.h>

// Headlights D2 (left, GPIO1) and D3 (right, GPIO20).
// Each LED sits between 3V3 (via 47 ohm) and its GPIO, so they are ACTIVE LOW:
// pin LOW = lit, pin HIGH = dark. PWM duty is inverted inside the module.
//
// Off after every wake-up. Switched and dimmed by the Connect (stick click
// cycles off/33/66/100 %) and the phone app (toggle + brightness slider).
//
// Runs in its own small FreeRTOS task (like Eyes), so fades and blinks stay
// smooth while main.cpp blocks. All calls are thread-safe and return at once.
//
// Layers, highest priority first:
//   1. one-shot patterns   blink(), flash()      e.g. "paired" = 3 fast blinks
//   2. pairing indicator   setPairing(true)      alternating left / right
//   3. the user's setting  setLevel() / toggle() / cycle()
//   4. sleep               fadeOut(), then dark
namespace Headlights {

enum Side : uint8_t { LEFT = 1, RIGHT = 2, BOTH = 3 };

void begin();                    // attach PWM, start the task, lights OFF

// ---- user control ----------------------------------------------------------
void setLevel(uint8_t pct);      // 0 = off, 1..100 = on at that brightness
uint8_t level();                 // current setting, 0 = off
void toggle();                   // off <-> last brightness used
uint8_t cycle();                 // off -> 33 -> 66 -> 100 -> off; returns the new level
// Brightness used by toggle() (remembered across sleep by main.cpp in NVS).
void setLastOnLevel(uint8_t pct);
uint8_t lastOnLevel();

// ---- indications -----------------------------------------------------------
void setPairing(bool on);        // pairing window open -> alternating blink
// One-shot pattern: interrupts whatever is showing, then the setting resumes.
void blink(uint8_t times, uint16_t onMs = 70, uint16_t offMs = 70, Side side = BOTH);
void flash(uint16_t ms = 250, Side side = BOTH);   // single longer flash

// ---- power -----------------------------------------------------------------
void fadeOut(uint16_t ms);       // with the sleep animation; stays dark afterwards
// Before deep sleep: stop the task, detach PWM, drive both pins HIGH (dark)
// and hold them so the LEDs cannot glow from a floating pin.
void prepareForSleep();
// First thing after wake-up: release those holds (before begin()).
void releaseSleepHold();

// Ready-made event cues, so main.cpp reads like a story.
inline void cuePaired()   { blink(3, 70, 70); }     // new Connect paired
inline void cueLinked()   { blink(2, 60, 90); }     // Connect or phone app connected
inline void cueLinkLost() { flash(400); }           // Connect or phone app gone
inline void cueMood()     { blink(1, 50, 60); }     // eye expression changed
inline void cueTrim()     { blink(1, 40, 40); }     // trim saved

}  // namespace Headlights
