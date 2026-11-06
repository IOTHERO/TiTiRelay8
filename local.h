// Local control over the home Wi-Fi (hub contract, docs/platform.md "Local control"):
// while the internet is down the app sends signed commands straight to the module.
//   GET  /local/v1/info     {"device_id", "firmware", "local"}
//   GET  /local/v1/nonce    {"device_id", "nonce", "ttl_s": 30}   (one use, 16 open at most)
//   POST /local/v1/command  {"nonce", "tag", "value_json", "sig"}
//   POST /local/v1/state    {"nonce", "sig"}
// sig = base64url(HMAC-SHA256(key, "<device_id>\n<nonce>\ncommand\n<tag>\n<value_json>")),
// key from the hub ({"type": "config", "local": {...}}), kept in NVS. Announced over mDNS as
// _titiswitch._tcp with TXT id=<device_id>. What was done is reported afterwards (local_log).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace local {
void begin();
void loop();
void applyConfig(JsonVariantConst local);   // {"enabled", "key", "version", "port"} (any task)
bool keySet();
uint16_t port();
String addr();                              // the module's private address, "" if none
// A change made without the server (local command or offline rule), reported later.
void log(uint8_t relay, bool value, const char* by);
void flushLog();                            // publish the waiting entries (platform connected)
}
