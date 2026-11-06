// DIP switch (address AD1..AD7, config AD8), debounced.
#pragma once
#include <Arduino.h>

namespace inputs {
void begin();
void loop();                 // samples every 10 ms; a change must hold for 50 ms
bool configAtBoot();         // read once in begin(): the config switch decides the mode
bool configOn();             // live, debounced
uint8_t address();           // AD1..AD7 as a number, AD1 = bit 0
uint8_t raw();               // bit 0..6 = AD1..AD7, bit 7 = config (AD8)
}
