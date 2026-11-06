// Config portal: soft AP at 192.168.1.100 while the config switch (DIP 8) was ON at power-up.
// Web UI with login, live state over a WebSocket, settings, relay/input test, firmware update.
#pragma once
#include <Arduino.h>

namespace portal {
void begin();
void loop();
}
