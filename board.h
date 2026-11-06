// Board definition: TiTi Relay-8 (ESP32-WROOM-32, ULN2803 relay driver, MAX485 / ADM2682E).
// Pin numbers follow the schematic (net names MCU_DIx, MCU_ADx, ADM_xxx).
#pragma once
#include <Arduino.h>

#define FW_NAME     "TiTi Relay-8"
#define FW_MODEL    "TR8-ESP32"
#define FW_VERSION  "1.0.0"
#define FW_VER_HEX  0x0100          // Modbus input register 3: major << 8 | minor

// GPIO12 drives relay 8 on the PCB but it is the MTDI strapping pin (flash voltage at boot).
// It stays unused: relay 8 is reported as "not fitted". Set to 1 only after checking that
// the ULN2803 input pull-down (10k) keeps GPIO12 low during reset on your board revision.
#ifndef ENABLE_RELAY8_GPIO12
#define ENABLE_RELAY8_GPIO12 0
#endif

static constexpr uint8_t RELAY_COUNT = 8;
// ULN2803 is inverting: GPIO HIGH = coil energised = relay ON. -1 = not fitted.
static constexpr int8_t RELAY_PINS[RELAY_COUNT] = {
  5, 18, 19, 21, 22, 23, 13, ENABLE_RELAY8_GPIO12 ? 12 : -1};

static constexpr uint8_t PIN_LED_DATA = 14;   // active high (2.2k to LED)
static constexpr uint8_t PIN_LED_RUN  = 27;

// RS-485 transceiver: DE and /RE tied together (ADM_SE).
static constexpr uint8_t PIN_RS485_SE = 4;
static constexpr uint8_t PIN_RS485_TX = 16;   // net ADM_TXD -> transceiver DI / TXD
static constexpr uint8_t PIN_RS485_RX = 17;   // net ADM_RXD <- transceiver RO / RXD

// DIP switch U6: COM to +3.3 V, 4.7k pull-down per line -> ON = HIGH.
// AD1..AD7 = Modbus address bits (AD1 = least significant), AD8 = config switch.
static constexpr uint8_t ADDR_BITS = 7;
static constexpr uint8_t PIN_ADDR[ADDR_BITS] = {36, 39, 34, 35, 32, 33, 25};
static constexpr uint8_t PIN_CONFIG = 26;

// Config portal (soft AP)
static constexpr uint8_t PORTAL_IP[4] = {192, 168, 1, 100};
