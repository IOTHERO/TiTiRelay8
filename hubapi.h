// HTTP(S) calls to the hub with the device credential (Settings::apiUrl, Bearer <secret>):
//   GET  /ingest/v1/devices/<id>/config          local key (after every start)
//   GET  /ingest/v1/devices/<id>/rules           rules for running cut off (after every start)
//   GET  /ingest/v1/devices/<id>/firmware/<rel>  a firmware offer's image
// Runs in its own task so a slow server never holds up the relays.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace hubapi {
void begin();
bool configured();                    // apiUrl and a platform credential are set
void fetchAll();                      // config + rules (after a connection to the platform)
void offerOta(JsonObjectConst offer); // MQTT d/<id>/ota (any task)
void reportPendingOta();              // after connecting: the result of an update before restart
bool otaBusy();
}
