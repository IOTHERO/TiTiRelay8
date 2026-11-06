// Persistent settings (NVS, one versioned blob) and their JSON form for the config portal.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "board.h"

enum LinkMode : uint8_t { MODE_UNSET = 0, MODE_WIFI = 1, MODE_RS485 = 2 };
enum PowerOn : uint8_t { PON_RESTORE = 0, PON_OFF = 1, PON_ON = 2 };
enum LinkLoss : uint8_t { LOSS_HOLD = 0, LOSS_ALL_OFF = 1, LOSS_PRESET = 2 };
enum ServerProto : uint8_t { PROTO_PLATFORM = 0, PROTO_GENERIC = 1 };

static constexpr uint8_t MAX_SERVERS = 4;
static constexpr uint8_t MAX_NETS = 2;

struct WifiNet {
  char ssid[33];
  char pass[65];
};

struct ServerCfg {
  bool enabled;
  uint8_t proto;          // ServerProto
  char name[24];
  char host[64];
  uint16_t port;
  bool tls;
  char user[64];          // platform: the device id
  char pass[96];          // platform: the device secret
  char clientId[48];      // empty = derived from the device id / MAC
  char prefix[64];        // platform: topic prefix (usually empty); generic: base topic
  uint16_t keepalive;
  uint8_t qos;
};

struct Settings {
  uint16_t version;
  char deviceName[32];
  uint8_t mode;           // LinkMode

  // Wi-Fi
  WifiNet nets[MAX_NETS];  // [0] main, [1] backup
  bool dhcp;
  uint32_t ip, gateway, subnet, dns1, dns2;   // IPAddress as uint32 (network order of IPAddress)
  char hostname[32];
  char ntp[48];

  // Servers (tried in order; the next one on failure)
  ServerCfg servers[MAX_SERVERS];
  char deviceId[32];      // platform device id used in the topics d/<id>/...
  uint16_t telemetryS;    // heartbeat period

  // RS-485 / Modbus RTU slave
  uint8_t mbAddrFromDip;  // 1 = address from DIP AD1..AD7, 0 = mbAddr
  uint8_t mbAddr;
  uint32_t mbBaud;
  uint8_t mbParity;       // 0 none, 1 even, 2 odd
  uint8_t mbStopBits;     // 1 or 2
  uint16_t mbRespDelayMs;
  bool mbSwapRxTx;

  // Link loss (Modbus silence or MQTT disconnected)
  uint16_t linkTimeoutS;  // 0 = off
  uint8_t linkLoss;       // LinkLoss
  uint8_t linkLossMask;   // relays ON for LOSS_PRESET

  // Relays
  uint8_t powerOn[RELAY_COUNT];
  uint8_t interlock[RELAY_COUNT];   // partner relay number 1..8, 0 = none
  char relayName[RELAY_COUNT][20];
  uint16_t minIntervalMs;           // per relay, between two changes
  uint16_t staggerMs;               // between two coils switched on

  // Portal / system
  char apPass[65];
  char adminPass[33];
  uint8_t logLevel;
};

namespace settings {
extern Settings cur;
void defaults(Settings& s);
void load();
bool save(const Settings& s);      // validates first; false (and nothing stored) if invalid
bool validate(Settings& s, String& err);
void toJson(const Settings& s, JsonObject out, bool withSecrets);
// Fill s from JSON (keys missing in json keep s's value; an empty password keeps the old one).
void fromJson(Settings& s, JsonObjectConst in);
void factoryReset();

// Shared CA certificate (PEM) for TLS servers; empty = the built-in CA bundle.
String caPem();
bool setCaPem(const String& pem);

uint8_t modbusAddress();           // effective address (DIP or manual)
String apSsid();
String defaultClientId();
}
