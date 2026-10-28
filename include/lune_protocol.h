// Lun-E shared protocol — used by the rover (Lun-E RC), the Connect controller
// and (mirrored in JavaScript) the Web Bluetooth app in app/index.html.
//
// Transport 1: ESP-NOW (Connect <-> rover), fixed Wi-Fi channel LUNE_ESPNOW_CHANNEL.
// Transport 2: BLE GATT (phone -> rover), service LUNE_BLE_SVC_UUID.
//
// Keep this file identical for both firmwares. Bump LUNE_PROTO_VERSION on any
// change to a packet layout.
#pragma once
#include <stdint.h>

#define LUNE_PROTO_VERSION 3  // v3: headlights back (lightsSeq in CONTROL, lights level in TELEMETRY + BLE status)
#define LUNE_MAGIC0 'L'
#define LUNE_MAGIC1 'E'

// All ESP-NOW traffic stays on this channel. Neither board joins a Wi-Fi network.
#define LUNE_ESPNOW_CHANNEL 1

// ---------------------------------------------------------------------------
// ESP-NOW packets
// ---------------------------------------------------------------------------
enum LunePktType : uint8_t {
  LUNE_PKT_PAIR_REQ  = 0x01,  // Connect -> broadcast: "pair with me"
  LUNE_PKT_PAIR_ACK  = 0x02,  // rover   -> Connect:   "paired"
  LUNE_PKT_CONTROL   = 0x10,  // Connect -> rover, ~50 Hz
  LUNE_PKT_TELEMETRY = 0x20,  // rover   -> Connect, ~5 Hz
};

// Eye expressions shown on the rover's OLED.
enum LuneMood : uint8_t {
  LUNE_MOOD_NORMAL = 0,
  LUNE_MOOD_HAPPY,
  LUNE_MOOD_SAD,
  LUNE_MOOD_ANGRY,
  LUNE_MOOD_SLEEPY,
  LUNE_MOOD_SURPRISED,
  LUNE_MOOD_COUNT
};

// Which input currently drives the motors.
enum LuneSource : uint8_t {
  LUNE_SRC_NONE    = 0,
  LUNE_SRC_CONNECT = 1,  // ESP-NOW
  LUNE_SRC_APP     = 2,  // BLE
};

#pragma pack(push, 1)
struct LuneHeader {
  uint8_t magic[2];  // 'L','E'
  uint8_t version;   // LUNE_PROTO_VERSION
  uint8_t type;      // LunePktType
  uint8_t seq;       // rolling counter
};

struct LunePairReq {
  LuneHeader h;
  char name[12];     // controller name, NUL padded
};

struct LunePairAck {
  LuneHeader h;
  char name[12];     // rover name, NUL padded
};

struct LuneControl {
  LuneHeader h;
  int8_t  x;          // steer    -100 (left) .. +100 (right)
  int8_t  y;          // throttle -100 (back) .. +100 (forward)
  uint8_t maxSpeed;   // 10..100 %
  uint8_t moodSeq;    // incremented once per "next expression" press
  uint8_t lightsSeq;  // incremented once per headlight press (rover cycles off/33/66/100 %)
};

// flags bits in LuneTelemetry / BLE status
#define LUNE_FLAG_LIGHTS      0x01  // headlights are on
#define LUNE_FLAG_APP_LINK    0x02  // a phone is connected over BLE
#define LUNE_FLAG_CONNECT_LINK 0x04 // Connect packets are arriving
#define LUNE_FLAG_PAIRING     0x08  // rover is in pairing mode
#define LUNE_FLAG_PAIRED      0x10  // rover has a stored Connect

struct LuneTelemetry {
  LuneHeader h;
  uint8_t flags;      // LUNE_FLAG_*
  uint8_t source;     // LuneSource
  uint8_t mood;       // LuneMood
  int8_t  left;       // applied motor output -100..100
  int8_t  right;
  int8_t  rssi;       // RSSI of the last Connect packet seen by the rover (dBm)
  uint8_t lights;     // headlight brightness 0 (off) .. 100 %
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// BLE GATT (phone app). UUIDs are random v4 values for Lun-E.
// ---------------------------------------------------------------------------
#define LUNE_BLE_NAME_PREFIX  "Lun-E"
#define LUNE_BLE_SVC_UUID     "6e4c0001-7a3d-4f8b-9c51-4c554e452d31"
// write-without-response, 3 bytes: int8 x, int8 y, uint8 maxSpeed
#define LUNE_BLE_DRIVE_UUID   "6e4c0002-7a3d-4f8b-9c51-4c554e452d31"
// write, 2 bytes: uint8 cmd, uint8/int8 arg (see LuneBleCmd)
#define LUNE_BLE_CMD_UUID     "6e4c0003-7a3d-4f8b-9c51-4c554e452d31"
// read + notify, LuneBleStatus (sent ~5 Hz while connected)
#define LUNE_BLE_STATUS_UUID  "6e4c0004-7a3d-4f8b-9c51-4c554e452d31"

enum LuneBleCmd : uint8_t {
  LUNE_CMD_LIGHTS  = 0x01,  // arg: 0 = off, 1..100 = on at that %, LUNE_LIGHTS_TOGGLE, LUNE_LIGHTS_CYCLE
  LUNE_CMD_MOOD    = 0x02,  // arg: LuneMood
  LUNE_CMD_TRIM    = 0x03,  // arg: int8 -30..30, + = steer right; saved to flash
  LUNE_CMD_PAIR    = 0x04,  // arg: ignored. Open a 30 s window to pair a Connect
  LUNE_CMD_SLEEP   = 0x05,  // arg: ignored. Deep sleep now (wake with the button)
};

// Special LUNE_CMD_LIGHTS arguments (int8).
#define LUNE_LIGHTS_TOGGLE ((int8_t)-1)  // off <-> last brightness
#define LUNE_LIGHTS_CYCLE  ((int8_t)-2)  // off -> 33 -> 66 -> 100 -> off (Connect stick click)

#pragma pack(push, 1)
struct LuneBleStatus {
  uint8_t  version;   // LUNE_PROTO_VERSION
  uint8_t  flags;     // LUNE_FLAG_*
  uint8_t  source;    // LuneSource
  uint8_t  mood;      // LuneMood
  int8_t   left;      // applied motor output -100..100
  int8_t   right;
  int8_t   trim;
  int8_t   connectRssi;
  uint16_t uptimeS;   // little endian
  uint8_t  lights;    // headlight brightness 0 (off) .. 100 % (v3; apps accept 10-byte v2 status)
};
#pragma pack(pop)

static inline void luneFillHeader(LuneHeader &h, uint8_t type, uint8_t seq) {
  h.magic[0] = LUNE_MAGIC0;
  h.magic[1] = LUNE_MAGIC1;
  h.version = LUNE_PROTO_VERSION;
  h.type = type;
  h.seq = seq;
}

static inline bool luneHeaderValid(const uint8_t *data, int len) {
  return len >= (int)sizeof(LuneHeader) && data[0] == LUNE_MAGIC0 &&
         data[1] == LUNE_MAGIC1 && data[2] == LUNE_PROTO_VERSION;
}
