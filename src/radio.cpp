#include "radio.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "control.h"

// Pairing, safe with a room full of rovers:
//   1. Connect broadcasts PAIR_REQ.
//   2. A rover answers with PAIR_ACK only if it is unpaired or in pairing mode
//      AND the request is strong (Connect held close, >= PAIR_RSSI_MIN), or if
//      the request comes from the Connect it already knows.
//   3. Connect waits a moment, picks the strongest ACK and starts sending
//      CONTROL to that one rover.
//   4. Only the rover that receives CONTROL from the candidate stores the
//      pairing. Every other rover that answered forgets it after a few seconds.

namespace {
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

char name[12] = "Lun-E";
uint8_t number = 0;
uint8_t peerMac[6] = {0};
volatile bool hasPeer = false;
volatile uint32_t pairingUntil = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Step 2: PAIR_REQ seen in the Wi-Fi task, answered from loop().
volatile bool pendingAck = false;
uint8_t pendingMac[6];
// Step 4: candidate Connect waiting for its first CONTROL.
uint8_t candMac[6];
volatile bool hasCand = false;
volatile uint32_t candUntil = 0;
volatile bool commitPending = false;
volatile bool pairedEvent = false;

volatile uint32_t lastConnectMs = 0;
volatile int8_t lastRssi = -127;
uint8_t lastMoodSeq = 0;
uint8_t lastLightsSeq = 0;
bool seqInit = false;
uint8_t txSeq = 0;

bool isPairing() { return (int32_t)(pairingUntil - millis()) > 0; }
bool isPeer(const uint8_t *mac) { return hasPeer && memcmp(mac, peerMac, 6) == 0; }

void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;  // = current channel (LUNE_ESPNOW_CHANNEL)
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void dropPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
}

void sendAck(const uint8_t *mac) {
  LunePairAck ack = {};
  luneFillHeader(ack.h, LUNE_PKT_PAIR_ACK, txSeq++);
  strncpy(ack.name, name, sizeof(ack.name));
  ack.number = number;
  esp_now_send(mac, (const uint8_t *)&ack, sizeof(ack));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!luneHeaderValid(data, len)) return;
  const uint8_t *src = info->src_addr;
  uint8_t type = ((const LuneHeader *)data)->type;
  int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : -127;

  if (type == LUNE_PKT_PAIR_REQ && len >= (int)sizeof(LunePairReq)) {
    bool open = !hasPeer || isPairing();
    if (isPeer(src) || (open && rssi >= PAIR_RSSI_MIN)) {
      portENTER_CRITICAL(&mux);
      memcpy(pendingMac, src, 6);
      pendingAck = true;
      portEXIT_CRITICAL(&mux);
    }
    return;
  }

  if (type == LUNE_PKT_CONTROL && len >= (int)sizeof(LuneControl)) {
    if (!isPeer(src)) {
      // First CONTROL from the Connect we just answered: it picked us.
      if (hasCand && memcmp(src, candMac, 6) == 0) commitPending = true;
      return;
    }
    const LuneControl *c = (const LuneControl *)data;
    lastConnectMs = millis();
    lastRssi = rssi;
    Control::submit(LUNE_SRC_CONNECT, c->x, c->y, c->maxSpeed);
    if (!seqInit) {
      lastMoodSeq = c->moodSeq;
      lastLightsSeq = c->lightsSeq;
      seqInit = true;
    }
    // Button presses travel as counters so a lost packet never loses or repeats a press.
    if (c->moodSeq != lastMoodSeq) {
      lastMoodSeq = c->moodSeq;
      Control::pushCmd(LUNE_CMD_MOOD, MOOD_ARG_NEXT, LUNE_SRC_CONNECT);
    }
    if (c->lightsSeq != lastLightsSeq) {
      lastLightsSeq = c->lightsSeq;
      Control::pushCmd(LUNE_CMD_LIGHTS, LUNE_LIGHTS_CYCLE, LUNE_SRC_CONNECT);
    }
  }
}
}  // namespace

namespace Radio {

void begin(const char *roverName, uint8_t roverNumber) {
  strncpy(name, roverName, sizeof(name) - 1);
  number = roverNumber;

  Preferences prefs;
  prefs.begin("lune", true);
  hasPeer = prefs.getBytes("peer", peerMac, 6) == 6;
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // Modem sleep must stay enabled while BLE is running (Wi-Fi/BT coexistence).
  esp_wifi_set_channel(LUNE_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    log_e("esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  addPeer(BROADCAST);
  if (hasPeer) addPeer(peerMac);
}

void loop() {
  // Step 2: answer a PAIR_REQ.
  if (pendingAck) {
    uint8_t mac[6];
    portENTER_CRITICAL(&mux);
    memcpy(mac, pendingMac, 6);
    pendingAck = false;
    portEXIT_CRITICAL(&mux);
    addPeer(mac);  // needed to unicast the ACK
    if (!isPeer(mac)) {
      if (hasCand && memcmp(mac, candMac, 6) != 0) dropPeer(candMac);
      memcpy(candMac, mac, 6);
      candUntil = millis() + PAIR_CANDIDATE_MS;
      hasCand = true;
    }
    sendAck(mac);
  }

  // Step 4: the Connect chose us — store it.
  if (commitPending) {
    commitPending = false;
    if (hasPeer) dropPeer(peerMac);
    memcpy(peerMac, candMac, 6);
    hasPeer = true;
    hasCand = false;
    seqInit = false;
    pairingUntil = 0;
    Preferences prefs;
    prefs.begin("lune", false);
    prefs.putBytes("peer", peerMac, 6);
    prefs.end();
    pairedEvent = true;
    log_i("Paired with %02X:%02X:%02X:%02X:%02X:%02X", peerMac[0], peerMac[1], peerMac[2],
          peerMac[3], peerMac[4], peerMac[5]);
  }

  // A Connect we answered picked another rover: forget it.
  if (hasCand && (int32_t)(millis() - candUntil) > 0) {
    hasCand = false;
    if (!isPeer(candMac)) dropPeer(candMac);
  }
}

bool paired() { return hasPeer; }
bool pairing() { return !hasPeer || isPairing(); }
bool pairingWindow() { return isPairing(); }
void startPairing(uint32_t ms) { pairingUntil = millis() + ms; }

bool connectLinked() {
  uint32_t t = lastConnectMs;
  return t != 0 && millis() - t < CONNECT_LINK_MS;
}

int8_t rssi() { return lastRssi; }

void sendTelemetry(LuneTelemetry &t) {
  if (!hasPeer) return;
  luneFillHeader(t.h, LUNE_PKT_TELEMETRY, txSeq++);
  t.number = number;
  esp_now_send(peerMac, (const uint8_t *)&t, sizeof(t));
}

bool takePairedEvent() {
  bool e = pairedEvent;
  pairedEvent = false;
  return e;
}

}  // namespace Radio
