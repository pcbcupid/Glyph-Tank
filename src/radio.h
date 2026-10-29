// ESP-NOW link to the Connect controller: pairing, control in, telemetry out.
#pragma once
#include <Arduino.h>
#include "lune_protocol.h"

namespace Radio {
void begin(const char *roverName, uint8_t roverNumber);  // name goes in PAIR_ACK
void loop();                        // finishes pairing (flash writes) outside the Wi-Fi task
bool paired();
bool pairing();
bool pairingWindow();     // only the timed window (double-click / app), not "unpaired"
void startPairing(uint32_t ms);
bool connectLinked();               // Connect heard within CONNECT_LINK_MS
int8_t rssi();                      // last Connect packet RSSI, dBm
void sendTelemetry(LuneTelemetry &t);
// Set when a pairing completed since the last call (for the "PAIRED" caption).
bool takePairedEvent();
}  // namespace Radio
