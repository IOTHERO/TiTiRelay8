// Watchdog, reset reason, crash-loop detection (safe mode) and memory watch.
#pragma once
#include <Arduino.h>

namespace health {
void begin();                  // before anything else that may hang
void watchThisTask();          // add the calling task to the task watchdog
void feed();                   // from every watched task's loop
void loop();
bool safeMode();               // 3 abnormal resets in a row: outputs start OFF
const char* resetReason();
uint32_t bootCount();
void restart(const char* why); // saves state, then restarts
}
