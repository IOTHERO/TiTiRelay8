// Wi-Fi station: main and backup network, static IP or DHCP, reconnect with back-off, NTP time.
#pragma once
#include <Arduino.h>

namespace net {
void begin(bool keepAp);     // keepAp: config portal stays up (AP + STA)
void loop();
void stop();
bool up();
int rssi();
String ip();
String ssid();
const char* status();        // short text for the portal
bool timeValid();
uint64_t epochMs();          // UTC; 0 if the time is not known
}
