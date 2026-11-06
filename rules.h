// Rules the module runs by itself while it is cut off from the server (hub contract,
// backend/api/home.py local_rules): {"type": "rules", "version", "rules": [...],
// "utc_offset_min", "holidays": ["YYYY-MM-DD", ...], "offline_after_s", "server_time"}.
// Kept in NVS; run only when the server has been unreachable for offline_after_s, so a rule
// never runs twice. Triggers: time, interval, value; conditions on the module's own values;
// window; actions write and delay. Days are Jalali (weekday 0 = Saturday), as on the server.
#pragma once
#include <Arduino.h>

namespace rules {
void begin();
void loop(bool serverReachable);
void apply(const char* json, size_t len);   // a rules message (any task)
uint8_t count();
const char* version();
}
