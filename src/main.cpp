// Lun-E Glyph rover firmware (ESP32-S3)
//
// Drive from either:
//   * Connect controller over ESP-NOW (paired, ~50 Hz)
//   * Phone web app over BLE (Web Bluetooth, app/index.html)
// The first source to move the stick takes control; the other can take over
// once the owner has been neutral for HANDOVER_IDLE_MS. Motors stop if the
// owner goes quiet for FAILSAFE_MS.
//
// Button (GPIO0):  click = next eye expression · double-click = show who I am
//                  (number big on the OLED) · triple-click = pair a Connect (30 s)
//                  hold 1.5 s = eyes fall asleep, release = deep sleep · press to wake.
// Rover number 1..99 is set from the app (Setup → rover number) and kept in
// flash; the rover then advertises as "Lun-E 07". Unnumbered rovers use the
// end of their MAC (e.g. "Lun-E Glyph-A3F1").
// Headlights (pins TBD, disabled): OFF after wake-up. Connect stick click cycles
//                  off/33/66/100 %; app has a toggle + brightness slider.
//                  Cues: alternate while pairing · 3 fast blinks paired · 2 blinks
//                  linked · long flash link lost · 1 blink new expression ·
//                  fade out with the sleep animation.

#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>
#include "ble_app.h"
#include "config.h"
#include "control.h"
#include "eyes.h"
#include "headlights.h"
#include "lune_protocol.h"
#include "motors.h"
#include "radio.h"

static char roverName[20];
static char roverId[8];        // "07" / "A3F1" — what the OLED shows for find-me
static uint8_t roverNumber = 0;
static uint32_t wiggleFrom = 0, wiggleUntil = 0;
static int8_t trim = 0;
static uint8_t owner = LUNE_SRC_NONE;
static uint32_t lastActivityMs = 0;
static void saveLightsIfSettled(bool force);

// ---------------------------------------------------------------------------
// Deep sleep
// ---------------------------------------------------------------------------
static void releaseSleepHolds() {
  gpio_hold_dis((gpio_num_t)PIN_MOT_STBY);
  gpio_hold_dis((gpio_num_t)PIN_BUTTON);
  Headlights::releaseSleepHold();
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_dis();
#endif
}

// Stops everything, plays the falling-asleep eyes (the OLED turns itself off
// at the end), waits for the button to be let go, then deep-sleeps.
// Called while the button is still held for a long-press sleep.
static void goToSleep() {
  Serial.println("[lune] deep sleep");
  Motors::stopNow();
  Headlights::fadeOut(HEADLIGHT_SLEEP_FADE_MS);   // fades with the eyes
  saveLightsIfSettled(true);                     // keep a just-chosen brightness
  Eyes::startSleep();
  // Wait for the button to be released so it does not wake us immediately.
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  Eyes::waitAsleep();
  delay(50);

  // Hold the motor driver asleep and the button pulled up through sleep.
  digitalWrite(PIN_MOT_STBY, LOW);
  gpio_pullup_en((gpio_num_t)PIN_BUTTON);
  gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
  gpio_hold_en((gpio_num_t)PIN_MOT_STBY);
  gpio_hold_en((gpio_num_t)PIN_BUTTON);
  Headlights::prepareForSleep();  // pins HIGH (dark) + held
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
  // ESP32-S3: wake on the button (an RTC GPIO) going LOW.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// Commands (from the app, Connect or the button)
// ---------------------------------------------------------------------------
// Headlights: LUNE_CMD_LIGHTS from the app (toggle / brightness) or the Connect
// (cycle). The chosen brightness is remembered in flash, but the lights always
// start OFF after a wake-up.
static uint8_t savedLightPct = 0;
static uint32_t lightChangedMs = 0;

static void handleLights(int8_t arg, uint8_t source) {
  uint8_t before = Headlights::level();
  if (arg == LUNE_LIGHTS_TOGGLE) Headlights::toggle();
  else if (arg == LUNE_LIGHTS_CYCLE) Headlights::cycle();
  else if (arg >= 0) Headlights::setLevel((uint8_t)arg);
  else return;
  uint8_t now = Headlights::level();
  lightChangedMs = millis() | 1;
  // Caption for button-style changes; the app's slider would spam it.
  if (arg < 0 && now != before) {
    char buf[16];
    if (now) snprintf(buf, sizeof(buf), "LIGHTS %u%%", now);
    else snprintf(buf, sizeof(buf), "LIGHTS OFF");
    Eyes::say(buf, 900);
  }
  (void)source;
}

// Save the brightness once it has stopped changing (slider drags = many writes).
static void saveLightsIfSettled(bool force) {
  if (!lightChangedMs || (!force && millis() - lightChangedMs < HEADLIGHT_SAVE_DELAY_MS)) return;
  lightChangedMs = 0;
  uint8_t pct = Headlights::lastOnLevel();
  if (pct == savedLightPct) return;
  savedLightPct = pct;
  Preferences prefs;
  prefs.begin("lune", false);
  prefs.putUChar("lightpct", pct);
  prefs.end();
}

// "Find me": number big on the OLED, headlights blink, a little shimmy.
static void identify() {
  Eyes::showId(roverId, IDENTIFY_MS);
  Headlights::blink(5, 90, 90);
  wiggleFrom = millis();
  wiggleUntil = wiggleFrom + IDENTIFY_WIGGLE_MS;
}

static void handleCmd(const RoverCmd &c) {
  switch (c.cmd) {
    case LUNE_CMD_MOOD:
      if (c.arg == MOOD_ARG_NEXT) Eyes::nextMood();
      else Eyes::setMood((uint8_t)c.arg);
      Headlights::cueMood();
      break;
    case LUNE_CMD_TRIM: {
      int8_t t = constrain(c.arg, -30, 30);
      if (t != trim) {
        trim = t;
        Preferences prefs;
        prefs.begin("lune", false);
        prefs.putChar("trim", trim);
        prefs.end();
        Headlights::cueTrim();
      }
      break;
    }
    case LUNE_CMD_LIGHTS:
      handleLights(c.arg, c.source);
      break;
    case LUNE_CMD_PAIR:
      Radio::startPairing(PAIRING_WINDOW_MS);
      Eyes::say("PAIRING...", 2000);
      break;
    case LUNE_CMD_SLEEP:
      goToSleep();
      break;
    case LUNE_CMD_IDENTIFY:
      identify();
      break;
    case LUNE_CMD_NUMBER: {
      uint8_t n = (uint8_t)c.arg;
      if (n > 99) break;
      Preferences prefs;
      prefs.begin("lune", false);
      prefs.putUChar("num", n);
      prefs.end();
      char id[8];
      if (n) snprintf(id, sizeof(id), "%02u", (unsigned)n);
      else snprintf(id, sizeof(id), "--");
      Eyes::showId(id, 2500);
      Headlights::cuePaired();
      // Restart so Bluetooth advertises the new name. Give the app's write a
      // moment to be acknowledged first.
      Motors::stopNow();
      saveLightsIfSettled(true);
      delay(1800);
      ESP.restart();
      break;
    }
  }
  lastActivityMs = millis();
}

// ---------------------------------------------------------------------------
// Button: click, double-click, long hold
// ---------------------------------------------------------------------------
// Set in setup() when the button press that woke us is still held, so that
// press is not also counted as a click.
static bool ignoreButtonUntilRelease = false;

static void pollButton() {
  static bool stable = false, lastRaw = false;
  static uint32_t changedAt = 0, pressedAt = 0, lastReleaseAt = 0;
  static uint8_t clicks = 0;
  uint32_t now = millis();

  bool raw = digitalRead(PIN_BUTTON) == LOW;
  if (ignoreButtonUntilRelease) {
    if (!raw) ignoreButtonUntilRelease = false;
    lastRaw = stable = false;
    return;
  }
  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt > 25 && raw != stable) {
    stable = raw;
    if (stable) {
      pressedAt = now;
    } else if (now - pressedAt < 600) {
      clicks++;
      lastReleaseAt = now;
    }
    lastActivityMs = now;
  }
  // Long press: start falling asleep right away, while it is still held.
  if (stable && now - pressedAt >= BUTTON_SLEEP_MS) {
    goToSleep();  // does not return
  }
  if (!stable && clicks && now - lastReleaseAt > 350) {
    if (clicks == 1) Control::pushCmd(LUNE_CMD_MOOD, MOOD_ARG_NEXT, LUNE_SRC_NONE);
    else if (clicks == 2) Eyes::showId(roverId, IDENTIFY_MS);  // "who am I": number on the OLED
    else Control::pushCmd(LUNE_CMD_PAIR, 0, LUNE_SRC_NONE);
    clicks = 0;
  }
}

// ---------------------------------------------------------------------------
// Source arbitration + mixing
// ---------------------------------------------------------------------------
static int applyDeadzone(int v) {
  if (abs(v) <= INPUT_DEADZONE) return 0;
  // Rescale so output still reaches 100 at full stick.
  int s = v > 0 ? 1 : -1;
  return s * (abs(v) - INPUT_DEADZONE) * 100 / (100 - INPUT_DEADZONE);
}

static void driveUpdate() {
  uint32_t now = millis();
  SourceInput in[3] = {SourceInput(), Control::snapshot(LUNE_SRC_CONNECT), Control::snapshot(LUNE_SRC_APP)};
  auto fresh = [&](uint8_t s) { return in[s].seen && now - in[s].lastMs < FAILSAFE_MS; };
  auto activeNow = [&](uint8_t s) { return fresh(s) && in[s].lastActiveMs == in[s].lastMs; };

  bool ownerFresh = owner != LUNE_SRC_NONE && fresh(owner);
  bool ownerIdle = !ownerFresh || now - in[owner].lastActiveMs >= HANDOVER_IDLE_MS;
  if (ownerIdle) {
    uint8_t other = owner == LUNE_SRC_APP ? LUNE_SRC_CONNECT : LUNE_SRC_APP;
    if (activeNow(other)) owner = other;
    else if (!ownerFresh) owner = activeNow(LUNE_SRC_CONNECT) ? LUNE_SRC_CONNECT
                                  : activeNow(LUNE_SRC_APP)   ? LUNE_SRC_APP
                                                              : LUNE_SRC_NONE;
  }

  int x = 0, y = 0, maxSpeed = 100;
  if (owner != LUNE_SRC_NONE && fresh(owner)) {
    x = applyDeadzone(in[owner].x);
    y = applyDeadzone(in[owner].y);
    maxSpeed = in[owner].maxSpeed;
    if (x || y) lastActivityMs = now;
  }

  // Arcade mix: throttle +/- turn, normalised so neither tread exceeds 100 %.
  float turn = x * TURN_GAIN / 100.0f;
  float l = y + turn, r = y - turn;
  float m = max(fabsf(l), fabsf(r));
  if (m > 100) { l = l * 100 / m; r = r * 100 / m; }
  // Trim: + makes the left tread faster (steers right) to cancel a drift left.
  l *= 1.0f + trim / 100.0f;
  r *= 1.0f - trim / 100.0f;
  float k = maxSpeed / 100.0f;
  if (!x && !y && (int32_t)(wiggleUntil - now) > 0) {
    // Find-me shimmy: twist left/right in place, only while nobody is driving.
    int dir = ((now - wiggleFrom) / 140) % 2 ? 1 : -1;
    Motors::setTarget(45 * dir, -45 * dir);
  } else {
    Motors::setTarget((int)lroundf(l * k), (int)lroundf(r * k));
  }

  Eyes::lookAt(x, y, x || y);
}

// ---------------------------------------------------------------------------
// Status out: telemetry to Connect, notify to the app
// ---------------------------------------------------------------------------
static uint8_t statusFlags() {
  uint8_t f = 0;
  if (BleApp::connected()) f |= LUNE_FLAG_APP_LINK;
  if (Radio::connectLinked()) f |= LUNE_FLAG_CONNECT_LINK;
  if (Radio::pairing()) f |= LUNE_FLAG_PAIRING;
  if (Headlights::level()) f |= LUNE_FLAG_LIGHTS;
  if (Radio::paired()) f |= LUNE_FLAG_PAIRED;
  return f;
}

static void publishStatus() {
  uint8_t flags = statusFlags();
  if (Radio::connectLinked() || Radio::pairing()) {
    LuneTelemetry t = {};
    t.flags = flags;
    t.source = owner;
    t.mood = Eyes::mood();
    t.left = Motors::left();
    t.right = Motors::right();
    t.rssi = Radio::rssi();
    t.lights = Headlights::level();
    Radio::sendTelemetry(t);
  }
  LuneBleStatus s = {};
  s.version = LUNE_PROTO_VERSION;
  s.flags = flags;
  s.source = owner;
  s.mood = Eyes::mood();
  s.left = Motors::left();
  s.right = Motors::right();
  s.trim = trim;
  s.connectRssi = Radio::rssi();
  s.uptimeS = (uint16_t)min<uint32_t>(millis() / 1000, 65535);
  s.lights = Headlights::level();
  s.number = roverNumber;
  BleApp::publishStatus(s);
}

// ---------------------------------------------------------------------------
void setup() {
  releaseSleepHolds();
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Motors::begin();
  Headlights::begin();

  Serial.begin(115200);
  delay(50);

  Preferences prefs;
  prefs.begin("lune", true);
  trim = prefs.getChar("trim", 0);
  roverNumber = prefs.getUChar("num", 0);
  if (roverNumber > 99) roverNumber = 0;
  Headlights::setLastOnLevel(prefs.getUChar("lightpct", HEADLIGHT_DEFAULT_PCT));
  prefs.end();
  savedLightPct = Headlights::lastOnLevel();   // lights themselves start OFF

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  luneRoverName(roverName, sizeof(roverName), "Glyph", roverNumber, mac);
  luneRoverId(roverId, sizeof(roverId), roverNumber, mac);
  // Short name for Connect's screen (PAIR_ACK holds 11 characters).
  char ackName[12];
  if (roverNumber) snprintf(ackName, sizeof(ackName), "%s", roverName);
  else snprintf(ackName, sizeof(ackName), "Glyph %s", roverId);
  Serial.printf("[lune] %s  MAC %02X:%02X:%02X:%02X:%02X:%02X\n", roverName, mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);

  Control::begin();
  bool oled = Eyes::begin();
  Serial.printf("[lune] OLED %s\n", oled ? "found" : "not found");
  Radio::begin(ackName, roverNumber);
  BleApp::begin(roverName);

  // The wake-up eye animation is the greeting; only nag when unpaired.
  if (!Radio::paired()) Eyes::say("PAIR ME", 2000);
  ignoreButtonUntilRelease = digitalRead(PIN_BUTTON) == LOW;
  lastActivityMs = millis();
}

void loop() {
  pollButton();

  RoverCmd c;
  while (Control::popCmd(c)) handleCmd(c);

  Radio::loop();
  // Only the timed pairing window (triple-click / app). Radio::pairing() is also
  // true whenever no Connect is paired yet, which would blink forever.
  Headlights::setPairing(Radio::pairingWindow());
  static uint32_t pairedAt = 0;
  if (Radio::takePairedEvent()) {
    Eyes::showId(roverId, 2000);   // which rover just paired
    Eyes::say("PAIRED!", 3500);    // shows under the eyes once the number goes
    Headlights::cuePaired();
    pairedAt = millis() | 1;
  }
  bool justPaired = pairedAt && millis() - pairedAt < 1500;

  static bool wasLinked = false;
  bool linked = Radio::connectLinked();
  if (linked != wasLinked) {
    wasLinked = linked;
    Eyes::say(linked ? "CONNECT" : "CONNECT LOST", 1000);
    if (!linked) Headlights::cueLinkLost();
    else if (!justPaired) Headlights::cueLinked();   // don't stomp the paired blinks
  }
  int ble = BleApp::takeLinkEvent();
  if (ble > 0) {
    Eyes::showId(roverId, 2000);   // "you connected to this one"
    Headlights::cueLinked();
  } else if (ble < 0) {
    Eyes::say("APP LEFT", 1000);
    Headlights::cueLinkLost();
  }

  driveUpdate();
  Motors::update();
  saveLightsIfSettled(false);

  static uint32_t lastStatus = 0;
  uint32_t now = millis();
  if (now - lastStatus >= TELEMETRY_MS) {
    lastStatus = now;
    publishStatus();
  }

  if (linked || BleApp::connected()) lastActivityMs = now;
  if (now - lastActivityMs > AUTO_SLEEP_MS) goToSleep();

  delay(5);  // ~200 Hz control loop
}
