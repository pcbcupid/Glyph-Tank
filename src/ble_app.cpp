#include "ble_app.h"
#include <NimBLEDevice.h>
#include "control.h"

namespace {
NimBLEServer *server = nullptr;
NimBLECharacteristic *statusChr = nullptr;
volatile int linkEvent = 0;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    // Snappy link for joystick traffic: 15–30 ms interval, no latency, 4 s timeout.
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    linkEvent = 1;
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    // Stop driving right away instead of waiting for the failsafe.
    Control::submit(LUNE_SRC_APP, 0, 0, 100);
    linkEvent = -1;
    NimBLEDevice::startAdvertising();
  }
};

class DriveCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 2) return;
    const uint8_t *d = v.data();
    uint8_t maxSpeed = v.size() >= 3 ? d[2] : 100;
    Control::submit(LUNE_SRC_APP, (int8_t)d[0], (int8_t)d[1], maxSpeed);
  }
};

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 1) return;
    const uint8_t *d = v.data();
    Control::pushCmd(d[0], v.size() >= 2 ? (int8_t)d[1] : 0, LUNE_SRC_APP);
  }
};

ServerCB serverCB;
DriveCB driveCB;
CmdCB cmdCB;
}  // namespace

namespace BleApp {

void begin(const char *deviceName) {
  NimBLEDevice::init(deviceName);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCB, false);

  NimBLEService *svc = server->createService(LUNE_BLE_SVC_UUID);
  NimBLECharacteristic *drive = svc->createCharacteristic(
      LUNE_BLE_DRIVE_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  drive->setCallbacks(&driveCB);
  NimBLECharacteristic *cmd = svc->createCharacteristic(
      LUNE_BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(&cmdCB);
  statusChr = svc->createCharacteristic(
      LUNE_BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  LuneBleStatus empty = {};
  empty.version = LUNE_PROTO_VERSION;
  statusChr->setValue((const uint8_t *)&empty, sizeof(empty));
  svc->start();

  // Advertising packet: flags + 128-bit service UUID (fits in 31 bytes).
  // Name goes in the scan response.
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(LUNE_BLE_SVC_UUID));
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName(deviceName);
  adv->setScanResponseData(scanData);
  adv->start();
}

bool connected() { return server && server->getConnectedCount() > 0; }

void publishStatus(const LuneBleStatus &s) {
  if (!statusChr) return;
  statusChr->setValue((const uint8_t *)&s, sizeof(s));
  if (connected()) statusChr->notify();
}

int takeLinkEvent() {
  int e = linkEvent;
  linkEvent = 0;
  return e;
}

}  // namespace BleApp
