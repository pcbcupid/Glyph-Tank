#include "control.h"
#include "config.h"

namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
SourceInput inputs[3];  // indexed by LuneSource
QueueHandle_t cmdQueue = nullptr;

int8_t clampPct(int v) { return (int8_t)constrain(v, -100, 100); }
}  // namespace

namespace Control {

void begin() { cmdQueue = xQueueCreate(16, sizeof(RoverCmd)); }

void submit(uint8_t source, int8_t x, int8_t y, uint8_t maxSpeed) {
  if (source != LUNE_SRC_CONNECT && source != LUNE_SRC_APP) return;
  uint32_t now = millis();
  x = clampPct(x);
  y = clampPct(y);
  bool active = abs(x) > INPUT_DEADZONE || abs(y) > INPUT_DEADZONE;
  portENTER_CRITICAL(&mux);
  SourceInput &in = inputs[source];
  in.x = x;
  in.y = y;
  in.maxSpeed = constrain(maxSpeed, 10, 100);
  in.lastMs = now;
  if (active) in.lastActiveMs = now;
  in.seen = true;
  portEXIT_CRITICAL(&mux);
}

void pushCmd(uint8_t cmd, int8_t arg, uint8_t source) {
  if (!cmdQueue) return;
  RoverCmd c{cmd, arg, source};
  xQueueSend(cmdQueue, &c, 0);  // never block a radio task
}

SourceInput snapshot(uint8_t source) {
  SourceInput s;
  if (source > LUNE_SRC_APP) return s;
  portENTER_CRITICAL(&mux);
  s = inputs[source];
  portEXIT_CRITICAL(&mux);
  return s;
}

bool popCmd(RoverCmd &out) {
  return cmdQueue && xQueueReceive(cmdQueue, &out, 0) == pdTRUE;
}

}  // namespace Control
