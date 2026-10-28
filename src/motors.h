#pragma once
#include <Arduino.h>

namespace Motors {
void begin();
// Set the target tread speeds, -100..100 %. Output ramps towards it.
void setTarget(int left, int right);
// Call often (every loop). Applies ramping, dead band and STBY control.
void update();
// Immediate stop and driver sleep (failsafe / deep sleep).
void stopNow();
int left();   // currently applied output, -100..100
int right();
}  // namespace Motors
