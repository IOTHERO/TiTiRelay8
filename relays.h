// Relay outputs. Every change, from any source or task, goes through post(): the main loop
// applies it with the per-relay minimum interval, the inrush stagger and the interlocks, and
// keeps the state in NVS so it survives a power cut.
#pragma once
#include <Arduino.h>
#include "board.h"

namespace relays {
enum Source : uint8_t { SRC_BOOT, SRC_WEB, SRC_MQTT, SRC_MODBUS, SRC_LINK, SRC_SYSTEM };
enum Op : uint8_t { OP_SET, OP_MASK, OP_ALL_OFF };

struct Cmd {
  uint8_t op;
  uint8_t relay;          // 0-based, OP_SET
  bool value;             // OP_SET
  uint8_t mask;           // OP_MASK: bit i = relay i ON
  uint8_t src;            // Source
  bool force;             // safety action: skips the minimum interval
  uint32_t pulseMs;       // OP_SET with value true: switch off again after this long
  char id[40];            // command id to report back (empty = none)
};

// Outcome of a command that carried an id (called from the main loop).
using ResultFn = void (*)(const char* id, bool ok, uint8_t relay, bool value, const char* err);

void begin(bool safeMode);
void loop();
bool post(const Cmd& c);                      // any task; false if the queue is full
bool set(uint8_t relay, bool on, Source src, uint32_t pulseMs = 0);
bool setMask(uint8_t mask, Source src);
bool allOff(Source src, bool force);
void onResult(ResultFn fn);

uint8_t state();                 // bit i = relay i energised now
uint8_t pulsing();
bool fitted(uint8_t relay);
uint8_t fittedMask();
uint32_t cycles(uint8_t relay);  // ON operations over the relay's life
uint32_t changes();              // increments on every change (telemetry trigger)
const char* sourceName(uint8_t src);
void saveNow();                  // before a planned restart
}
