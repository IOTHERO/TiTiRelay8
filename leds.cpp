#include "leds.h"
#include "board.h"

namespace leds {
namespace {
Pattern pattern = RUN_BOOT;
volatile uint32_t dataUntil = 0;
uint32_t identifyUntil = 0;

// On/off steps in 100 ms units, repeated.
bool runLevel(uint32_t now) {
  uint32_t t = now / 100;
  switch (pattern) {
    case RUN_BOOT: return true;
    case RUN_CONFIG: return (now / 125) & 1;
    case RUN_OK: return (t % 20) < 10;
    case RUN_NO_LINK: { uint32_t k = t % 20; return k == 0 || k == 3; }
    case RUN_FAULT: { uint32_t k = t % 20; return k == 0 || k == 3 || k == 6; }
  }
  return false;
}
}  // namespace

void begin() {
  pinMode(PIN_LED_RUN, OUTPUT);
  pinMode(PIN_LED_DATA, OUTPUT);
  digitalWrite(PIN_LED_RUN, HIGH);
  digitalWrite(PIN_LED_DATA, LOW);
}

void loop() {
  uint32_t now = millis();
  if (identifyUntil && (int32_t)(identifyUntil - now) > 0) {
    bool on = (now / 80) & 1;
    digitalWrite(PIN_LED_RUN, on);
    digitalWrite(PIN_LED_DATA, !on);
    return;
  }
  identifyUntil = 0;
  digitalWrite(PIN_LED_RUN, runLevel(now));
  digitalWrite(PIN_LED_DATA, (int32_t)(dataUntil - now) > 0);
}

void run(Pattern p) { pattern = p; }
void data() { dataUntil = millis() + 40; }
void identify(uint32_t ms) { identifyUntil = millis() + ms; }
}  // namespace leds
