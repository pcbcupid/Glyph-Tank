#include "eyes.h"
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"
#include "lune_protocol.h"

namespace {
#if OLED_ROTATION == 2
#define U8_ROT U8G2_R2
#else
#define U8_ROT U8G2_R0
#endif
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8_ROT, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);

bool hasOled = false;
TaskHandle_t task = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Shared state (written by the main loop, read by the eye task)
volatile uint8_t curMood = LUNE_MOOD_NORMAL;
volatile int lookTX = 0, lookTY = 0;
volatile bool lookActive = false;
char caption[16] = "";
uint32_t captionUntil = 0;
char bigId[8] = "";
uint32_t bigFrom = 0, bigUntil = 0;
volatile bool wantSleep = false;
volatile bool isAsleep = false;
volatile uint32_t wakeAnimEnd = 0;  // millis() when the wake animation finishes

// Animation state (eye task only)
enum Anim : uint8_t { ANIM_NONE, ANIM_WAKE, ANIM_SLEEP };
Anim anim = ANIM_NONE;
uint32_t animStart = 0;
float lookX = 0, lookY = 0;          // smoothed, -1..1
float wanderX = 0, wanderY = 0;
uint32_t nextWander = 0, nextBlink = 0, blinkStart = 0;
bool blinking = false;
float openness = 1.0f;               // 1 open, 0 closed

const int EYE_W = 36, EYE_H = 36, EYE_R = 9, EYE_GAP = 22;
const int CX = 64, CY = 30;
const int EYE_LX = CX - (EYE_W + EYE_GAP) / 2;  // left eye centre x
const int EYE_RX = CX + (EYE_W + EYE_GAP) / 2;  // right eye centre x

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// One eye at centre (cx, cy). side: -1 left eye, +1 right eye.
//   open   0..~1.15  height fraction
//   wScale 0..1      width fraction (eyes grow from a dot when waking)
//   size   ~1        overall scale (stretch / surprise)
//   droop  0..1      1 = keep the bottom edge fixed so the lid comes down from the top
void drawEye(int cx, int cy, int side, uint8_t mood, float open, float wScale = 1.0f,
             float size = 1.0f, float droop = 0.0f) {
  int w = (int)(EYE_W * size * wScale), h = (int)(EYE_H * size), r = (int)(EYE_R * size);
  if (mood == LUNE_MOOD_SURPRISED) { w += 6; h += 8; r += 3; }
  if (mood == LUNE_MOOD_SLEEPY) { open *= 0.45f; droop = 1.0f; }
  if (w < 2) return;
  int hh = max(2, (int)(h * open));
  int x0 = cx - w / 2;
  int yCentre = cy - hh / 2, yBottom = cy + h / 2 - hh - 2;
  int y0 = yCentre + (int)lroundf((yBottom - yCentre) * droop);
  int rr = min(r, min((hh - 1) / 2, (w - 1) / 2));
  oled.setDrawColor(1);
  if (rr >= 1) oled.drawRBox(x0, y0, w, hh, rr);
  else oled.drawBox(x0, y0, w, hh);

  // Lids are drawn in black on top of the eye.
  oled.setDrawColor(0);
  int x1 = x0 + w - 1;
  int lid = hh / 2;
  switch (mood) {
    case LUNE_MOOD_HAPPY:
      // Bottom cut by a disc -> cheerful "^ ^" arcs.
      oled.drawDisc(cx, y0 + hh + w * 0.42f, w * 0.72f);
      break;
    case LUNE_MOOD_SAD:  // outer corner droops
      if (side < 0) oled.drawTriangle(x0 - 1, y0 - 1, x1 + 1, y0 - 1, x0 - 1, y0 + lid);
      else          oled.drawTriangle(x0 - 1, y0 - 1, x1 + 1, y0 - 1, x1 + 1, y0 + lid);
      break;
    case LUNE_MOOD_ANGRY:  // inner corner pushed down
      if (side < 0) oled.drawTriangle(x0 - 1, y0 - 1, x1 + 1, y0 - 1, x1 + 1, y0 + lid);
      else          oled.drawTriangle(x0 - 1, y0 - 1, x1 + 1, y0 - 1, x0 - 1, y0 + lid);
      break;
    default:
      break;
  }
  oled.setDrawColor(1);
}

void drawPair(float lx, float ly, uint8_t mood, float open, float wScale = 1.0f,
              float size = 1.0f, float droop = 0.0f) {
  int dx = (int)(lx * 12), dy = (int)(ly * 8);
  drawEye(EYE_LX + dx, CY + dy, -1, mood, open, wScale, size, droop);
  drawEye(EYE_RX + dx, CY + dy, +1, mood, open, wScale, size, droop);
}

// ---------------------------------------------------------------------------
// Keyframes: piecewise smooth interpolation between (time ms, value) points.
// ---------------------------------------------------------------------------
struct Key { uint16_t t; float v; };

float track(const Key *k, int n, uint32_t t) {
  if (t <= k[0].t) return k[0].v;
  for (int i = 0; i < n - 1; ++i) {
    if (t < k[i + 1].t) {
      float u = float(t - k[i].t) / float(k[i + 1].t - k[i].t);
      u = u * u * (3 - 2 * u);  // ease in-out
      return k[i].v + (k[i + 1].v - k[i].v) * u;
    }
  }
  return k[n - 1].v;
}
#define TRACK(keys, t) track(keys, sizeof(keys) / sizeof(keys[0]), t)

// ---- Wake up: dark -> two slits -> groggy half-open, droop, try again ->
//      blink -> stretch wide open -> settle -> look left, right, centre.
const uint16_t WAKE_MS = 3000;
const Key WAKE_W[]    = {{250, 0.0f}, {600, 1.0f}};
const Key WAKE_OPEN[] = {{250, 0.04f}, {600, 0.04f}, {1000, 0.38f}, {1250, 0.12f}, {1500, 0.50f},
                         {1580, 0.05f}, {1750, 1.12f}, {1950, 1.0f}};
const Key WAKE_SIZE[] = {{1580, 1.0f}, {1750, 1.10f}, {1950, 1.0f}};
const Key WAKE_DROOP[] = {{1500, 1.0f}, {1750, 0.0f}};  // heavy lids until the big blink
const Key WAKE_LX[]   = {{1950, 0.0f}, {2250, -1.0f}, {2350, -1.0f}, {2650, 1.0f}, {2750, 1.0f}, {3000, 0.0f}};

void renderWake(uint32_t t) {
  oled.clearBuffer();
  if (t >= 250) {
    drawPair(TRACK(WAKE_LX, t), 0, LUNE_MOOD_NORMAL, TRACK(WAKE_OPEN, t), TRACK(WAKE_W, t),
             TRACK(WAKE_SIZE, t), TRACK(WAKE_DROOP, t));
  }
  oled.sendBuffer();
}

// ---- Fall asleep: lids get heavy, eyes sink, one nod back awake, slow close
//      to a slit, slit shrinks away, "z Z" float up, panel off.
const uint16_t SLEEP_MS = 2400;
const Key SLEEP_OPEN[] = {{0, 1.0f}, {450, 0.55f}, {700, 0.30f}, {900, 0.55f}, {1500, 0.04f}};
const Key SLEEP_W[]    = {{1500, 1.0f}, {1900, 0.0f}};
const Key SLEEP_LY[]   = {{0, 0.0f}, {900, 0.2f}, {1500, 0.5f}};

void drawFloatingZ(uint32_t t, uint16_t start, uint16_t dur, int x0, int y0, int x1, int y1,
                   const uint8_t *font, const char *s) {
  if (t < start || t >= start + dur) return;
  float u = float(t - start) / dur;
  oled.setFont(font);
  oled.drawStr(x0 + (int)((x1 - x0) * u), y0 + (int)((y1 - y0) * u), s);
}

void renderSleep(uint32_t t) {
  oled.clearBuffer();
  drawPair(lookX * (1.0f - min(1.0f, t / 600.0f)), TRACK(SLEEP_LY, t), LUNE_MOOD_NORMAL,
           TRACK(SLEEP_OPEN, t), TRACK(SLEEP_W, t), 1.0f, 1.0f);
  drawFloatingZ(t, 1100, 1100, 98, 26, 106, 10, u8g2_font_6x10_tr, "z");
  drawFloatingZ(t, 1450, 950, 106, 34, 116, 14, u8g2_font_9x15B_tr, "Z");
  oled.sendBuffer();
}

// ---- Rover number, big: "lun-e" label + the number, with a pop-in ----------
void drawId(const char *id, uint32_t t, float left) {
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(2, 9, "lun-e");
  int n = strlen(id);
  oled.setFont(n <= 2 ? u8g2_font_logisoso46_tr : u8g2_font_logisoso30_tr);
  int w = oled.getStrWidth(id);
  int baseline = n <= 2 ? 58 : 50;
  int x = (128 - w) / 2;
  if (t < 120) x += (int)((120 - t) / 120.0f * 12) * ((t / 40) % 2 ? 1 : -1);  // little shake in
  oled.drawStr(x, baseline, id);
  // bar along the bottom that shrinks as the time runs out
  int bw = (int)(128 * left);
  if (bw > 0) oled.drawHLine((128 - bw) / 2, 63, bw);
}

// ---- Normal running frame ----------------------------------------------------
void renderFrame(uint32_t now) {
  if (!blinking && now >= nextBlink) { blinking = true; blinkStart = now; }
  if (blinking) {
    uint32_t t = now - blinkStart;  // 0..180 ms: close 70, hold 30, open 80
    if (t < 70) openness = 1.0f - t / 70.0f;
    else if (t < 100) openness = 0.0f;
    else if (t < 180) openness = (t - 100) / 80.0f;
    else {
      openness = 1.0f;
      blinking = false;
      nextBlink = now + 1800 + (esp_random() % 4200);
      if ((esp_random() % 6) == 0) nextBlink = now + 250;  // occasional double blink
    }
  }

  float tx, ty;
  if (lookActive) {
    tx = lookTX / 100.0f;
    ty = -lookTY / 250.0f;  // throttle forward -> look slightly up
  } else {
    if (now >= nextWander) {
      wanderX = ((int)(esp_random() % 201) - 100) / 140.0f;
      wanderY = ((int)(esp_random() % 201) - 100) / 300.0f;
      if ((esp_random() % 3) == 0) wanderX = wanderY = 0;
      nextWander = now + 1200 + (esp_random() % 2800);
    }
    tx = wanderX;
    ty = wanderY;
  }
  float k = lookActive ? 0.35f : 0.18f;
  lookX += (tx - lookX) * k;
  lookY += (ty - lookY) * k;

  char id[8];
  bool showId;
  float idLeft = 0;
  portENTER_CRITICAL(&mux);
  showId = (int32_t)(now - bigFrom) >= 0 && (int32_t)(bigUntil - now) > 0 && bigId[0];
  if (showId) {
    memcpy(id, bigId, sizeof(id));
    idLeft = float(bigUntil - now) / float(max<uint32_t>(1, bigUntil - bigFrom));
  }
  portEXIT_CRITICAL(&mux);

  oled.clearBuffer();
  if (showId) {
    drawId(id, now - bigFrom, idLeft);
    oled.sendBuffer();
    return;
  }
  drawPair(lookX, lookY, curMood, openness);

  char cap[16];
  bool showCap;
  portENTER_CRITICAL(&mux);
  showCap = (int32_t)(captionUntil - now) > 0 && caption[0];
  if (showCap) memcpy(cap, caption, sizeof(cap));
  portEXIT_CRITICAL(&mux);
  if (showCap) {
    oled.setFont(u8g2_font_6x10_tr);
    int w = oled.getStrWidth(cap);
    int x = (128 - w) / 2;
    oled.setDrawColor(0);
    oled.drawBox(x - 3, 53, w + 6, 11);
    oled.setDrawColor(1);
    oled.drawStr(x, 62, cap);
  }
  oled.sendBuffer();
}

void eyeTask(void *) {
  const TickType_t period = pdMS_TO_TICKS(1000 / EYES_FPS);
  TickType_t last = xTaskGetTickCount();
  anim = ANIM_WAKE;
  animStart = millis();
  for (;;) {
    uint32_t now = millis();
    if (wantSleep && anim != ANIM_SLEEP) {
      anim = ANIM_SLEEP;
      animStart = now;
    }
    uint32_t t = now - animStart;
    switch (anim) {
      case ANIM_WAKE:
        if (t < WAKE_MS) {
          renderWake(t);
          break;
        }
        anim = ANIM_NONE;
        lookX = lookY = 0;
        nextBlink = now + 1500;
        nextWander = now + 800;
        [[fallthrough]];  // straight into a normal frame
      case ANIM_NONE:
        renderFrame(now);
        break;
      case ANIM_SLEEP:
        if (t < SLEEP_MS) {
          renderSleep(t);
          break;
        }
        oled.clearBuffer();
        oled.sendBuffer();
        oled.setPowerSave(1);
        isAsleep = true;
        vTaskSuspend(nullptr);
        break;
    }
    vTaskDelayUntil(&last, period);
  }
}
}  // namespace

namespace Eyes {

bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(OLED_ADDR);
  hasOled = (Wire.endTransmission() == 0);
  if (!hasOled) return false;
  oled.setI2CAddress(OLED_ADDR << 1);
  oled.setBusClock(400000);
  oled.begin();
  oled.clearBuffer();
  oled.sendBuffer();
  oled.setPowerSave(0);
  oled.setContrast(200);
  wakeAnimEnd = millis() + WAKE_MS + 50;
  xTaskCreate(eyeTask, "eyes", 4096, nullptr, 1, &task);
  return true;
}

bool present() { return hasOled; }

void setMood(uint8_t m) {
  if (m < LUNE_MOOD_COUNT) curMood = m;
}
uint8_t mood() { return curMood; }
void nextMood() { curMood = (curMood + 1) % LUNE_MOOD_COUNT; }

void lookAt(int x, int y, bool active) {
  lookTX = x;
  lookTY = y;
  lookActive = active;
}

void say(const char *text, uint32_t ms) {
  uint32_t now = millis();
  int32_t wait = (int32_t)(wakeAnimEnd - now);
  portENTER_CRITICAL(&mux);
  strncpy(caption, text, sizeof(caption) - 1);
  caption[sizeof(caption) - 1] = 0;
  captionUntil = now + (wait > 0 ? wait : 0) + ms;
  portEXIT_CRITICAL(&mux);
}

void showId(const char *id, uint32_t ms) {
  uint32_t now = millis();
  int32_t wait = (int32_t)(wakeAnimEnd - now);
  portENTER_CRITICAL(&mux);
  strncpy(bigId, id, sizeof(bigId) - 1);
  bigId[sizeof(bigId) - 1] = 0;
  bigFrom = now + (wait > 0 ? wait : 0);
  bigUntil = bigFrom + ms;
  portEXIT_CRITICAL(&mux);
}

void startSleep() {
  if (hasOled) wantSleep = true;
}

void waitAsleep(uint32_t timeoutMs) {
  if (!hasOled) return;
  uint32_t t0 = millis();
  while (!isAsleep && millis() - t0 < timeoutMs) delay(10);
}

}  // namespace Eyes
