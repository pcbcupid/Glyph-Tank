#pragma once
#include <Arduino.h>

// Animated eyes on the 0.96" SSD1306 OLED (J5). Runs in its own FreeRTOS task,
// so drawing never delays motor control. Safe no-op if no OLED is fitted.
namespace Eyes {
// Starts the eye task and plays the wake-up animation (groggy open, blink,
// stretch, look around). Returns false if no OLED answers on I2C.
bool begin();
bool present();
void setMood(uint8_t mood);           // LuneMood
uint8_t mood();
void nextMood();
// Where the eyes look: -100..100 each. Pass driving input; idle -> wander.
void lookAt(int x, int y, bool active);
// Short caption under the eyes, e.g. "PAIRED". Shown for ms. Captions asked
// for during the wake-up animation appear once it has finished.
void say(const char *text, uint32_t ms = 1200);
// Show the rover's number big in place of the eyes for ms, e.g. "07" or
// "A3F1" — used for "find me", when a phone connects and after pairing.
// Waits for the wake-up animation if it is still playing.
void showId(const char *id, uint32_t ms = 3000);
// Start the falling-asleep animation (lids droop, nod, close, "z z"), after
// which the panel turns off. Returns immediately; safe to call twice.
void startSleep();
// Block until the sleep animation has finished and the panel is off.
void waitAsleep(uint32_t timeoutMs = 4000);
}  // namespace Eyes
