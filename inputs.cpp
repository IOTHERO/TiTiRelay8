#include "inputs.h"
#include "board.h"
#include "log.h"

namespace inputs {
namespace {
constexpr uint32_t SAMPLE_MS = 10;
constexpr uint32_t STABLE_MS = 50;

volatile uint8_t stable = 0;
uint8_t candidate = 0;
uint32_t candidateSince = 0;
uint32_t lastSample = 0;
bool atBoot = false;

uint8_t sample() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < ADDR_BITS; i++)
    if (digitalRead(PIN_ADDR[i])) v |= 1 << i;
  if (digitalRead(PIN_CONFIG)) v |= 0x80;
  return v;
}
}  // namespace

void begin() {
  // External 4.7k pull-downs; GPIO34..39 have no internal pulls anyway.
  for (uint8_t i = 0; i < ADDR_BITS; i++) pinMode(PIN_ADDR[i], INPUT);
  pinMode(PIN_CONFIG, INPUT);
  // Majority of 9 reads, 5 ms apart, so a bouncing switch at power-up does not decide the mode.
  uint8_t votes[8] = {0};
  for (uint8_t n = 0; n < 9; n++) {
    uint8_t v = sample();
    for (uint8_t b = 0; b < 8; b++) if (v & (1 << b)) votes[b]++;
    delay(5);
  }
  uint8_t v = 0;
  for (uint8_t b = 0; b < 8; b++) if (votes[b] >= 5) v |= 1 << b;
  stable = candidate = v;
  atBoot = v & 0x80;
}

void loop() {
  uint32_t now = millis();
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;
  uint8_t v = sample();
  if (v != candidate) {
    candidate = v;
    candidateSince = now;
  } else if (v != stable && now - candidateSince >= STABLE_MS) {
    uint8_t changed = v ^ stable;
    stable = v;
    if (changed & 0x7F) LOGI("dip", "address switches now %u", v & 0x7F);
    if (changed & 0x80) LOGI("dip", "config switch %s", (v & 0x80) ? "ON" : "OFF");
  }
}

bool configAtBoot() { return atBoot; }
bool configOn() { return stable & 0x80; }
uint8_t address() { return stable & 0x7F; }
uint8_t raw() { return stable; }
}  // namespace inputs
