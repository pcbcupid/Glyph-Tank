// BLE GATT server for the phone app (Web Bluetooth). See lune_protocol.h.
#pragma once
#include <Arduino.h>
#include "lune_protocol.h"

namespace BleApp {
void begin(const char *deviceName);
bool connected();
void publishStatus(const LuneBleStatus &s);  // updates the value and notifies
// Set on connect / disconnect since the last call (for captions).
int takeLinkEvent();  // 0 none, 1 connected, -1 disconnected
}  // namespace BleApp
