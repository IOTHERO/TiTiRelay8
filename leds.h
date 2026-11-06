// RUN LED patterns and DATA LED activity flashes.
#pragma once
#include <Arduino.h>

namespace leds {
enum Pattern : uint8_t {
  RUN_BOOT,       // steady on
  RUN_CONFIG,     // fast blink (config portal)
  RUN_OK,         // slow blink, link up
  RUN_NO_LINK,    // double blink: Wi-Fi/server down or no Modbus master
  RUN_FAULT,      // triple blink: safe mode or invalid address
};
void begin();
void loop();
void run(Pattern p);
void data();                     // one activity flash (any task)
void identify(uint32_t ms);      // both LEDs flash fast
}
