# Lun-E Glyph firmware

Firmware for Lun-E Glyph, the tank built on the [Glyph ESP32-S3](https://learn.pcbcupid.com/documentation/modules/glyph/glyph-esp32s3/glyph-s3-overview) with a TB6612FNG motor driver. It is the Lun-E RC rover firmware ported to the S3.

It can be driven from the Lun-E **Connect** thumbstick controller (ESP-NOW, pair first) or the **phone app** (Web Bluetooth, https://whybit.in/drive/). Whoever moves their stick first takes control; the motors stop if the driving side goes quiet for 0.3 s.

The tank advertises as `Lun-E Glyph-XXXX`, where XXXX is the end of its MAC address.

## Build and flash

Uses **pioarduino** (Arduino-ESP32 core 3.x); the URL is in `platformio.ini`. The stock PlatformIO `espressif32` platform is still on core 2.x and will not build this.

```
pio run -t upload && pio device monitor
```

If the upload can't connect, hold BOOT while you plug in USB. Libraries (NimBLE-Arduino 2.x, U8g2) download automatically.

## Pins

| Function | GPIO |
|---|---|
| Button (also BOOT), active low, wakes from sleep | 0 |
| OLED SDA / SCL | 4 / 5 |
| TB6612FNG AIN1 / AIN2 / PWMA | 39 / 38 / 41 |
| TB6612FNG BIN1 / BIN2 / PWMB | 47 / 48 / 40 |
| TB6612FNG STBY | 42 |
| Gate | 34 |

All pins and tuning are in `src/config.h`.

## Not done yet

- **Headlights** are disabled (`HEADLIGHTS_ENABLED 0`) until their Glyph pins are set.
- **Gate (GPIO34)** is defined but not used by the firmware yet.
- **Motor direction** flags (`MOTOR_A_IS_LEFT`, `MOTOR_LEFT_INVERT`, `MOTOR_RIGHT_INVERT`) are copied from the RC. Check on the bench with the treads off the ground: push the stick forward and both treads should drive forward.

## Protocol

`include/lune_protocol.h` holds the packet formats and BLE UUIDs shared with Connect and the app. Keep it identical to the copy in the Lun-E firmware, and flash both boards after changing it.
