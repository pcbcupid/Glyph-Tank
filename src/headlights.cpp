#include "headlights.h"
#include <driver/gpio.h>
#include "config.h"

namespace {

const uint32_t PWM_MAX = (1u << HEADLIGHT_PWM_BITS) - 1;
const uint8_t CYCLE_STEPS[] = HEADLIGHT_CYCLE_STEPS;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t task = nullptr;
bool attached = false;

// ---- shared state (guarded by mux) ----------------------------------------
uint8_t levelPct = 0;                       // the user's setting, 0 = off
uint8_t lastOnPct = HEADLIGHT_DEFAULT_PCT;  // used by toggle()
bool pairing = false;
bool fading = false;                        // sleep fade in progress / finished
uint32_t fadeStartMs = 0;
uint16_t fadeMs = 0;
float fadeFrom = 0;

struct Pattern {
  bool active = false;
  uint8_t times = 0;
  uint16_t onMs = 0, offMs = 0;
  uint8_t mask = Headlights::BOTH;
  uint32_t startMs = 0;
} pat;

float outL = 0, outR = 0;   // what is on the LEDs now, 0..1 (perceived)

// ---- output (task only) ----------------------------------------------------
void writePins(float l, float r) {
  auto duty = [](float v) {
    v = constrain(v, 0.0f, 1.0f);
    return PWM_MAX - (uint32_t)(v * v * PWM_MAX + 0.5f);   // gamma 2, then active-low invert
  };
  ledcWrite(PIN_HEADLIGHT_L, duty(l));
  ledcWrite(PIN_HEADLIGHT_R, duty(r));
}

float approach(float cur, float target, float step) {
  if (target > cur) return min(target, cur + step);
  return max(target, cur - step);
}

// Target brightness for both sides. Returns true if the change should be
// instant (blinks, pairing, sleep fade) instead of smoothed.
bool computeTargets(uint32_t now, float &tl, float &tr) {
  if (fading) {
    float k = fadeMs ? (now - fadeStartMs) / (float)fadeMs : 1.0f;
    if (k > 1.0f) k = 1.0f;
    float e = 1.0f - k;
    tl = tr = fadeFrom * e * e;     // ease out
    return true;
  }

  // 3. the user's setting
  tl = tr = levelPct / 100.0f;
  bool instant = false;

  // 2. pairing: alternate left / right (full brightness so it reads in daylight)
  if (pairing) {
    bool leftOn = (now / HEADLIGHT_PAIRING_HALF_MS) % 2 == 0;
    tl = leftOn ? 1.0f : 0.0f;
    tr = leftOn ? 0.0f : 1.0f;
    instant = true;
  }

  // 1. one-shot pattern
  if (pat.active) {
    uint32_t period = (uint32_t)pat.onMs + pat.offMs;
    uint32_t t = now - pat.startMs;
    if (period == 0 || t >= period * pat.times) {
      pat.active = false;
    } else {
      bool on = (t % period) < pat.onMs;
      if (pat.mask & Headlights::LEFT) tl = on ? 1.0f : 0.0f;
      if (pat.mask & Headlights::RIGHT) tr = on ? 1.0f : 0.0f;
      instant = true;
    }
  }
  return instant;
}

void taskFn(void *) {
  TickType_t wake = xTaskGetTickCount();
  const float step = 1000.0f / HEADLIGHT_SMOOTH_MS / HEADLIGHT_TASK_HZ;  // full swing in SMOOTH_MS
  for (;;) {
    uint32_t now = millis();
    float tl, tr;
    portENTER_CRITICAL(&mux);
    bool instant = computeTargets(now, tl, tr);
    float l = instant ? tl : approach(outL, tl, step);
    float r = instant ? tr : approach(outR, tr, step);
    outL = l;
    outR = r;
    portEXIT_CRITICAL(&mux);
    writePins(l, r);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / HEADLIGHT_TASK_HZ));
  }
}

void setPinsDarkDigital() {
  pinMode(PIN_HEADLIGHT_L, OUTPUT);
  pinMode(PIN_HEADLIGHT_R, OUTPUT);
  digitalWrite(PIN_HEADLIGHT_L, HIGH);
  digitalWrite(PIN_HEADLIGHT_R, HIGH);
}

uint8_t clampPct(int p) { return (uint8_t)constrain(p, 0, 100); }

}  // namespace

namespace Headlights {

void begin() {
#if HEADLIGHTS_ENABLED
  setPinsDarkDigital();
  attached = ledcAttach(PIN_HEADLIGHT_L, HEADLIGHT_PWM_FREQ, HEADLIGHT_PWM_BITS) &&
             ledcAttach(PIN_HEADLIGHT_R, HEADLIGHT_PWM_FREQ, HEADLIGHT_PWM_BITS);
  if (!attached) {
    Serial.println("[lune] headlights: no free LEDC channel");
    return;
  }
  writePins(0, 0);
  xTaskCreate(taskFn, "lights", 2048, nullptr, 1, &task);
#endif
}

// ---- user control ------------------------------------------------------------
void setLevel(uint8_t pct) {
  pct = clampPct(pct);
  portENTER_CRITICAL(&mux);
  levelPct = pct;
  if (pct) lastOnPct = pct;
  portEXIT_CRITICAL(&mux);
}

uint8_t level() {
  portENTER_CRITICAL(&mux);
  uint8_t l = levelPct;
  portEXIT_CRITICAL(&mux);
  return l;
}

void toggle() {
  portENTER_CRITICAL(&mux);
  levelPct = levelPct ? 0 : (lastOnPct ? lastOnPct : HEADLIGHT_DEFAULT_PCT);
  portEXIT_CRITICAL(&mux);
}

uint8_t cycle() {
  portENTER_CRITICAL(&mux);
  uint8_t next = 0;
  if (levelPct == 0) {
    next = CYCLE_STEPS[0];
  } else {
    for (uint8_t s : CYCLE_STEPS) {   // first step brighter than now; past the last -> off
      if (s > levelPct) { next = s; break; }
    }
  }
  levelPct = next;
  if (next) lastOnPct = next;
  portEXIT_CRITICAL(&mux);
  return next;
}

void setLastOnLevel(uint8_t pct) {
  pct = clampPct(pct);
  if (!pct) return;
  portENTER_CRITICAL(&mux);
  lastOnPct = pct;
  portEXIT_CRITICAL(&mux);
}

uint8_t lastOnLevel() {
  portENTER_CRITICAL(&mux);
  uint8_t l = lastOnPct;
  portEXIT_CRITICAL(&mux);
  return l;
}

// ---- indications -------------------------------------------------------------
void setPairing(bool on) {
  portENTER_CRITICAL(&mux);
  pairing = on;
  portEXIT_CRITICAL(&mux);
}

void blink(uint8_t times, uint16_t onMs, uint16_t offMs, Side side) {
  if (!times || !onMs) return;
  portENTER_CRITICAL(&mux);
  if (!fading) {                      // nothing while falling asleep
    pat.active = true;
    pat.times = times;
    pat.onMs = onMs;
    pat.offMs = offMs;
    pat.mask = side;
    pat.startMs = millis();
  }
  portEXIT_CRITICAL(&mux);
}

void flash(uint16_t ms, Side side) { blink(1, ms, 0, side); }

// ---- power -------------------------------------------------------------------
void fadeOut(uint16_t ms) {
  portENTER_CRITICAL(&mux);
  fadeFrom = max(outL, outR);
  fading = true;
  fadeStartMs = millis();
  fadeMs = ms;
  pairing = false;
  pat.active = false;
  portEXIT_CRITICAL(&mux);
}

void prepareForSleep() {
  if (task) vTaskSuspend(task);
  if (attached) {
    ledcDetach(PIN_HEADLIGHT_L);
    ledcDetach(PIN_HEADLIGHT_R);
    attached = false;
  }
#if HEADLIGHTS_ENABLED
  setPinsDarkDigital();
  gpio_hold_en((gpio_num_t)PIN_HEADLIGHT_L);
  gpio_hold_en((gpio_num_t)PIN_HEADLIGHT_R);
#endif
}

void releaseSleepHold() {
#if HEADLIGHTS_ENABLED
  setPinsDarkDigital();   // level first, so releasing the hold cannot flash the LEDs
  gpio_hold_dis((gpio_num_t)PIN_HEADLIGHT_L);
  gpio_hold_dis((gpio_num_t)PIN_HEADLIGHT_R);
#endif
}

}  // namespace Headlights
