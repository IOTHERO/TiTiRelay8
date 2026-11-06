// MQTT link to the servers (tried in order, the next one after a failure).
//
// Platform protocol (the hub's native device contract, backend/mqtt_ingest.py):
//   <prefix>d/<id>/telemetry  {"ts": <epoch ms>, "values": {"relay_1": true, ...}}
//   <prefix>d/<id>/state      {"state": "online" | "offline"}   (retained, also the LWT)
//   <prefix>d/<id>/ack        {"command_id", "status": "received"|"done"|"failed", "value", "error"}
//   <prefix>d/<id>/pairing    {}
//   <prefix>d/<id>/cmd        {"command_id", "tag": "relay_1", "value": true, "expires_at", "pulse_s"}
//   Username = device id, password = device secret.
// Generic MQTT (e.g. the hub's mqtt_json driver): <base>/telemetry (same JSON), <base>/status
// ("online"/"offline" text), <base>/cmd ({"relay_1": true} or the platform command), <base>/ack.
#pragma once
#include <Arduino.h>

namespace cloud {
void begin();
void loop();
void stop();
bool connected();
int serverIndex();             // -1 if none
const char* lastError();
uint32_t downSinceMs();        // millis() when the link went down, 0 while connected
void pairing();                // the owner asks to pair (platform servers)
uint32_t rxCount();
uint32_t txCount();
// relays::onResult target: "done"/"failed" acks for platform commands.
void ackResult(const char* id, bool ok, uint8_t relay, bool value, const char* error);

// Config portal: one connection attempt to server idx, result in testResult().
void test(uint8_t idx);
String testResult();
}
