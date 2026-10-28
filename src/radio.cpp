#include "radio.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "control.h"

namespace {
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

char name[12] = "Lun-E Glyph";
uint8_t peerMac[6] = {0};
volatile bool hasPeer = false;
volatile uint32_t pairingUntil = 0;

// Pairing request captured in the Wi-Fi task, completed in loop().
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool pendingPair = false;
uint8_t pendingMac[6];
volatile bool pairedEvent = false;

volatile uint32_t lastConnectMs = 0;
volatile int8_t lastRssi = -127;
uint8_t lastMoodSeq = 0;
uint8_t lastLightsSeq = 0;
bool seqInit = false;
uint8_t txSeq = 0;

bool isPairing() { return (int32_t)(pairingUntil - millis()) > 0; }

void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;  // = current channel (LUNE_ESPNOW_CHANNEL)
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void sendAck(const uint8_t *mac) {
  LunePairAck ack = {};
  luneFillHeader(ack.h, LUNE_PKT_PAIR_ACK, txSeq++);
  strncpy(ack.name, name, sizeof(ack.name));
  esp_now_send(mac, (const uint8_t *)&ack, sizeof(ack));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!luneHeaderValid(data, len)) return;
  const uint8_t *src = info->src_addr;
  uint8_t type = ((const LuneHeader *)data)->type;

  if (type == LUNE_PKT_PAIR_REQ && len >= (int)sizeof(LunePairReq)) {
    // Accept when unpaired or in pairing mode, or re-ack the Connect we already know.
    bool known = hasPeer && memcmp(src, peerMac, 6) == 0;
    if (!hasPeer || isPairing() || known) {
      portENTER_CRITICAL(&mux);
      memcpy(pendingMac, src, 6);
      pendingPair = true;
      portEXIT_CRITICAL(&mux);
    }
    return;
  }

  if (type == LUNE_PKT_CONTROL && len >= (int)sizeof(LuneControl)) {
    if (!hasPeer || memcmp(src, peerMac, 6) != 0) return;  // not our controller
    const LuneControl *c = (const LuneControl *)data;
    lastConnectMs = millis();
    if (info->rx_ctrl) lastRssi = info->rx_ctrl->rssi;
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

void begin(const char *roverName) {
  strncpy(name, roverName, sizeof(name) - 1);

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
  if (!pendingPair) return;
  uint8_t mac[6];
  portENTER_CRITICAL(&mux);
  memcpy(mac, pendingMac, 6);
  pendingPair = false;
  portEXIT_CRITICAL(&mux);

  bool changed = !hasPeer || memcmp(mac, peerMac, 6) != 0;
  if (changed) {
    if (hasPeer && esp_now_is_peer_exist(peerMac)) esp_now_del_peer(peerMac);
    memcpy(peerMac, mac, 6);
    hasPeer = true;
    seqInit = false;
    Preferences prefs;
    prefs.begin("lune", false);
    prefs.putBytes("peer", peerMac, 6);
    prefs.end();
    pairedEvent = true;
    log_i("Paired with %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  addPeer(mac);
  pairingUntil = 0;
  sendAck(mac);
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
  esp_now_send(peerMac, (const uint8_t *)&t, sizeof(t));
}

bool takePairedEvent() {
  bool e = pairedEvent;
  pairedEvent = false;
  return e;
}

}  // namespace Radio
