// Thread-safe hand-off between the radio callbacks (ESP-NOW runs in the Wi-Fi
// task, BLE in the NimBLE host task) and the main loop.
#pragma once
#include <Arduino.h>
#include "lune_protocol.h"

struct SourceInput {
  int8_t x = 0, y = 0;
  uint8_t maxSpeed = 100;
  uint32_t lastMs = 0;        // last packet from this source
  uint32_t lastActiveMs = 0;  // last packet with non-neutral input
  bool seen = false;
};

struct RoverCmd {
  uint8_t cmd;    // LuneBleCmd
  int8_t arg;
  uint8_t source; // LuneSource
};

// Special MOOD argument meaning "next expression".
#define MOOD_ARG_NEXT ((int8_t)-1)

namespace Control {
void begin();
// Called from radio callbacks.
void submit(uint8_t source, int8_t x, int8_t y, uint8_t maxSpeed);
void pushCmd(uint8_t cmd, int8_t arg, uint8_t source);
// Called from the main loop.
SourceInput snapshot(uint8_t source);
bool popCmd(RoverCmd &out);
}  // namespace Control
