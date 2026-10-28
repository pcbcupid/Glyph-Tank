// Lun-E Glyph — board pins and tuning. Copied from Lun-E RC (schematic v1.5); update for the Glyph board.
#pragma once
#include <Arduino.h>

// ---- Pins (Glyph ESP32-S3) --------------------------------------------------
#define PIN_AIN1        39   // TB6612FNG motor A direction
#define PIN_AIN2        38
#define PIN_PWMA        41   // TB6612FNG motor A speed (PWM)
#define PIN_BIN1        47   // TB6612FNG motor B direction
#define PIN_BIN2        48
#define PIN_PWMB        40   // TB6612FNG motor B speed (PWM)
#define PIN_MOT_STBY    42   // TB6612FNG STBY, HIGH = motors enabled
#define PIN_GATE        34   // TODO: purpose not wired up in firmware yet
// TODO: headlight pins for Glyph. These are the RC's; GPIO20 is USB D+ on the
// S3, so HEADLIGHTS_ENABLED is 0 below until the real pins are set.
#define PIN_HEADLIGHT_L 1    // ACTIVE LOW (pin LOW = lit)
#define PIN_HEADLIGHT_R 20
#define PIN_BUTTON      0    // user button (also BOOT), active LOW, deep-sleep wake
#define PIN_I2C_SDA     4    // OLED
#define PIN_I2C_SCL     5
#define OLED_ADDR       0x3C

// ---- Motor wiring --------------------------------------------------------
// Which TB6612FNG channel drives which tread, and whether it spins backwards.
// Test once on the bench: push the stick forward — both treads must drive
// forward. Fix with these four flags, no rewiring needed.
// Copied from Lun-E RC (bench-tested 2026-10-26), not yet checked on Glyph:
// channel B drives the left tread and spins it backwards; channel A drives
// the right tread the right way round.
#define MOTOR_A_IS_LEFT   0
#define MOTOR_LEFT_INVERT  1
#define MOTOR_RIGHT_INVERT 0

// ---- Motor tuning ----------------------------------------------------------
#define MOTOR_PWM_FREQ     20000  // Hz, above hearing
#define MOTOR_PWM_BITS     10
#define MOTOR_MIN_DUTY     28     // % — N20 dead band; smallest non-zero command maps here
#define MOTOR_SLEW_PER_S   400    // % per second — limits current spikes on the 300 mAh cell
#define MOTOR_IDLE_SLEEP_MS 1500  // drop STBY after this long at zero output
#define TURN_GAIN          70     // % — how strongly the stick's X axis turns
#define INPUT_DEADZONE     6      // % — stick values below this are treated as 0

// ---- Control / safety ------------------------------------------------------
#define FAILSAFE_MS        300    // stop if the active source goes quiet this long
#define HANDOVER_IDLE_MS   300    // other source may take over after the owner is neutral this long
#define CONNECT_LINK_MS    1000   // Connect counts as "linked" if heard within this
#define PAIRING_WINDOW_MS  30000
#define TELEMETRY_MS       200    // rover -> Connect / app status rate

// ---- Power -----------------------------------------------------------------
#define AUTO_SLEEP_MS      (5UL * 60UL * 1000UL)  // idle with no Connect and no phone
#define BUTTON_SLEEP_MS    1500   // hold button this long, release -> deep sleep

// ---- Headlights ------------------------------------------------------------
// OFF after every wake-up. Connect: stick click cycles off -> 33 -> 66 -> 100 %.
// App: lights button (toggle) + brightness slider.
// Cues on top of the setting: 3 fast blinks = paired, 2 = Connect/app connected,
// long flash = link lost, 1 short = new eye expression / trim; alternating
// left/right while the pairing window is open; fade out with the sleep animation.
// Set HEADLIGHTS_ENABLED to 0 on boards built without D2/D3/R11/R12.
#define HEADLIGHTS_ENABLED        0      // TODO: set to 1 once Glyph headlight pins are known
#define HEADLIGHT_DEFAULT_PCT     66     // first "toggle on" brightness until one is chosen
#define HEADLIGHT_CYCLE_STEPS     {33, 66, 100}   // Connect stick-click steps, %
#define HEADLIGHT_SAVE_DELAY_MS   2000   // brightness saved to flash this long after the last change
#define HEADLIGHT_PAIRING_HALF_MS 250    // pairing blink: each side on this long
#define HEADLIGHT_SLEEP_FADE_MS   1800   // sleep: fade-out time (sleep animation is 2.4 s)
#define HEADLIGHT_SMOOTH_MS       200    // brightness changes ease over this long
#define HEADLIGHT_TASK_HZ         100
#define HEADLIGHT_PWM_FREQ        MOTOR_PWM_FREQ   // shares the motor timer (2 motor + 2 light LEDC channels)
#define HEADLIGHT_PWM_BITS        MOTOR_PWM_BITS

// ---- Display ---------------------------------------------------------------
// 0 = normal, 2 = rotated 180° (if the eyes appear upside down).
#define OLED_ROTATION      0
#define EYES_FPS           30
