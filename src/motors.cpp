#include "motors.h"
#include "config.h"

namespace {
const uint32_t PWM_MAX = (1u << MOTOR_PWM_BITS) - 1;

struct Channel {
  uint8_t in1, in2, pwm;
  bool invert;
  float out = 0;  // applied, -100..100
};

#if MOTOR_A_IS_LEFT
Channel chL{PIN_AIN1, PIN_AIN2, PIN_PWMA, MOTOR_LEFT_INVERT};
Channel chR{PIN_BIN1, PIN_BIN2, PIN_PWMB, MOTOR_RIGHT_INVERT};
#else
Channel chL{PIN_BIN1, PIN_BIN2, PIN_PWMB, MOTOR_LEFT_INVERT};
Channel chR{PIN_AIN1, PIN_AIN2, PIN_PWMA, MOTOR_RIGHT_INVERT};
#endif

float targetL = 0, targetR = 0;
uint32_t lastUpdateUs = 0;
uint32_t lastNonZeroMs = 0;
bool driverAwake = false;

void setDriverAwake(bool awake) {
  if (awake == driverAwake) return;
  driverAwake = awake;
  digitalWrite(PIN_MOT_STBY, awake ? HIGH : LOW);
}

// Drive one H-bridge. pct: -100..100, already ramped.
void writeChannel(const Channel &c, float pct) {
  if (c.invert) pct = -pct;
  float mag = fabsf(pct);
  if (mag < 0.5f) {  // coast: IN1 = IN2 = LOW
    digitalWrite(c.in1, LOW);
    digitalWrite(c.in2, LOW);
    ledcWrite(c.pwm, 0);
    return;
  }
  // Map 1..100 % into MOTOR_MIN_DUTY..100 % so small stick moves still turn the N20.
  float duty = MOTOR_MIN_DUTY + (100.0f - MOTOR_MIN_DUTY) * (mag / 100.0f);
  uint32_t on = (uint32_t)(duty * PWM_MAX / 100.0f);
  if (on > PWM_MAX) on = PWM_MAX;
  // TB6612FNG: IN1/IN2 set the direction, PWM sets the speed. While PWM is
  // low the bridge short-brakes (slow decay, better low-speed torque).
  digitalWrite(c.in1, pct > 0 ? HIGH : LOW);
  digitalWrite(c.in2, pct > 0 ? LOW : HIGH);
  ledcWrite(c.pwm, on);
}

float approach(float cur, float target, float step) {
  if (target > cur) return min(target, cur + step);
  return max(target, cur - step);
}
}  // namespace

namespace Motors {

void begin() {
  pinMode(PIN_MOT_STBY, OUTPUT);
  digitalWrite(PIN_MOT_STBY, LOW);
  driverAwake = false;
  const uint8_t dirPins[4] = {PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2};
  for (uint8_t p : dirPins) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  const uint8_t pwmPins[2] = {PIN_PWMA, PIN_PWMB};
  for (uint8_t p : pwmPins) {
    ledcAttach(p, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
    ledcWrite(p, 0);
  }
  lastUpdateUs = micros();
}

void setTarget(int left, int right) {
  targetL = constrain(left, -100, 100);
  targetR = constrain(right, -100, 100);
}

void update() {
  uint32_t now = micros();
  float dt = (now - lastUpdateUs) / 1e6f;
  lastUpdateUs = now;
  if (dt > 0.1f) dt = 0.1f;
  float step = MOTOR_SLEW_PER_S * dt;

  // Reversing direction: pass through zero first (ramp handles it naturally).
  chL.out = approach(chL.out, targetL, step);
  chR.out = approach(chR.out, targetR, step);

  bool moving = fabsf(chL.out) >= 0.5f || fabsf(chR.out) >= 0.5f;
  uint32_t ms = millis();
  if (moving) {
    lastNonZeroMs = ms;
    setDriverAwake(true);
  } else if (driverAwake && ms - lastNonZeroMs > MOTOR_IDLE_SLEEP_MS) {
    setDriverAwake(false);
  }
  writeChannel(chL, chL.out);
  writeChannel(chR, chR.out);
}

void stopNow() {
  targetL = targetR = 0;
  chL.out = chR.out = 0;
  writeChannel(chL, 0);
  writeChannel(chR, 0);
  setDriverAwake(false);
}

int left() { return (int)lroundf(chL.out); }
int right() { return (int)lroundf(chR.out); }

}  // namespace Motors
