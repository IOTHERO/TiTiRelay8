// Modbus RTU slave on RS-485 (own task). Register map: README.md.
//
// Coils           0..7   relays 1..8 (FC1, FC5, FC15)
// Discrete inputs 0..6   DIP AD1..AD7, 7 = config switch (FC2)
// Holding         0      relay mask (bit 0 = relay 1)            (FC3, FC6, FC16)
//                 1      command: 1 = all OFF, 2 = identify (LEDs flash 10 s)
//                 2..9   pulse relay 1..8, value in 0.1 s (write starts the pulse)
//                 10..19 reserved
//                 20..27 relay 1..8, one register each: 0 = OFF, 1 = ON (PLCs without coils)
// Input           0      relay mask now                           (FC4)
//                 1      DIP raw (bit 7 = config)    2  slave address
//                 3      firmware version (major << 8 | minor)
//                 4, 5   uptime s (32-bit; high word first unless "word swap")
//                 6  good frames   7  CRC errors
//                 8      boot count                  9  safe mode (1)
//                 10..25 ON operations relay 1..8 (32-bit each, same word order)
#pragma once
#include <Arduino.h>

namespace modbus {
void begin();
void stop();
uint8_t address();
uint32_t lastFrameMs();      // millis() of the last valid request to this slave, 0 = never
uint32_t goodFrames();
uint32_t crcErrors();
}
