#include "modbus.h"
#include <esp_task_wdt.h>
#include "board.h"
#include "health.h"
#include "inputs.h"
#include "leds.h"
#include "log.h"
#include "relays.h"
#include "settings.h"

namespace modbus {
namespace {
constexpr uint8_t HOLDING_COUNT = 28;
constexpr uint8_t RELAY_REGS = 20;      // holding 20..27: one register per relay (0/1)
constexpr uint8_t INPUT_COUNT = 26;
constexpr uint8_t EX_FUNCTION = 1, EX_ADDRESS = 2, EX_VALUE = 3;

HardwareSerial& port = Serial2;
TaskHandle_t task = nullptr;
volatile bool running = false;
uint8_t addr = 0;
uint32_t silenceUs = 1750;
uint8_t buf[256];
uint16_t len = 0;
uint32_t lastByteUs = 0;
bool discard = false;              // skip until the line is quiet (frame for someone else)
volatile uint32_t lastValid = 0;
uint32_t good = 0, crcErr = 0;   // statistics only

uint16_t crc16(const uint8_t* p, uint16_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= *p++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

uint16_t be16(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }

void send(uint8_t* out, uint16_t n) {
  uint16_t c = crc16(out, n);
  out[n++] = c & 0xFF;
  out[n++] = c >> 8;
  if (settings::cur.mbRespDelayMs) vTaskDelay(pdMS_TO_TICKS(settings::cur.mbRespDelayMs));
  digitalWrite(PIN_RS485_SE, HIGH);
  port.write(out, n);
  port.flush();                                  // waits until the last stop bit is out
  digitalWrite(PIN_RS485_SE, LOW);
  while (port.available()) port.read();          // anything seen while driving the line
  leds::data();
}

void exception(uint8_t fn, uint8_t code) {
  uint8_t out[5] = {addr, (uint8_t)(fn | 0x80), code};
  send(out, 3);
}

bool readBit(uint8_t table, uint16_t a) {
  if (table == 1) return (relays::state() >> a) & 1;
  return (inputs::raw() >> a) & 1;
}

uint16_t holding(uint16_t a) {
  if (a == 0) return relays::state();
  if (a >= RELAY_REGS && a < RELAY_REGS + RELAY_COUNT) return (relays::state() >> (a - RELAY_REGS)) & 1;
  return 0;
}

// The two registers of a 32-bit value: high word first, or low word first (mbWordSwap).
uint16_t word32(uint32_t v, bool first) {
  bool high = first != settings::cur.mbWordSwap;
  return high ? v >> 16 : v & 0xFFFF;
}

uint16_t inputReg(uint16_t a) {
  uint32_t up = millis() / 1000;
  switch (a) {
    case 0: return relays::state();
    case 1: return inputs::raw();
    case 2: return addr;
    case 3: return FW_VER_HEX;
    case 4: return word32(up, true);
    case 5: return word32(up, false);
    case 6: return good & 0xFFFF;
    case 7: return crcErr & 0xFFFF;
    case 8: return health::bootCount() & 0xFFFF;
    case 9: return health::safeMode();
  }
  if (a >= 10 && a < 26) {
    uint32_t c = relays::cycles((a - 10) / 2);
    return word32(c, ((a - 10) & 1) == 0);
  }
  return 0;
}

// 0 = done, else the Modbus exception code.
uint8_t writeHolding(uint16_t a, uint16_t v) {
  if (a == 0) {
    if (v > 0xFF) return EX_VALUE;
    relays::setMask(v, relays::SRC_MODBUS);
  } else if (a == 1) {
    if (v == 1) relays::allOff(relays::SRC_MODBUS, false);
    else if (v == 2) leds::identify(10000);
    else if (v != 0) return EX_VALUE;
  } else if (a >= 2 && a < 2 + RELAY_COUNT) {
    uint8_t r = a - 2;
    if (v) relays::set(r, true, relays::SRC_MODBUS, (uint32_t)v * 100);
    else relays::set(r, false, relays::SRC_MODBUS);
  } else if (a >= RELAY_REGS && a < RELAY_REGS + RELAY_COUNT) {
    if (v > 1) return EX_VALUE;
    relays::set(a - RELAY_REGS, v == 1, relays::SRC_MODBUS);
  } else {
    return EX_ADDRESS;                          // 10..19 reserved
  }
  return 0;
}

void handle(const uint8_t* f, uint16_t n, bool broadcast) {
  uint8_t fn = f[1];
  uint8_t out[256];
  out[0] = addr;
  out[1] = fn;
  switch (fn) {
    case 1:
    case 2: {                                     // read coils / discrete inputs
      uint16_t a = be16(f + 2), q = be16(f + 4);
      if (broadcast) return;
      if (q < 1 || q > 2000) return exception(fn, EX_VALUE);
      if (a + q > 8) return exception(fn, EX_ADDRESS);
      uint8_t bytes = (q + 7) / 8;
      out[2] = bytes;
      memset(out + 3, 0, bytes);
      for (uint16_t i = 0; i < q; i++)
        if (readBit(fn, a + i)) out[3 + i / 8] |= 1 << (i % 8);
      return send(out, 3 + bytes);
    }
    case 3:
    case 4: {                                     // read holding / input registers
      uint16_t a = be16(f + 2), q = be16(f + 4);
      if (broadcast) return;
      if (q < 1 || q > 125) return exception(fn, EX_VALUE);
      if (a + q > (fn == 3 ? HOLDING_COUNT : INPUT_COUNT)) return exception(fn, EX_ADDRESS);
      out[2] = q * 2;
      for (uint16_t i = 0; i < q; i++) {
        uint16_t v = fn == 3 ? holding(a + i) : inputReg(a + i);
        out[3 + i * 2] = v >> 8;
        out[4 + i * 2] = v & 0xFF;
      }
      return send(out, 3 + q * 2);
    }
    case 5: {                                     // write single coil
      uint16_t a = be16(f + 2), v = be16(f + 4);
      if (v != 0xFF00 && v != 0x0000) return broadcast ? void() : exception(fn, EX_VALUE);
      if (a >= RELAY_COUNT) return broadcast ? void() : exception(fn, EX_ADDRESS);
      relays::set(a, v == 0xFF00, relays::SRC_MODBUS);
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 6: {                                     // write single register
      uint16_t a = be16(f + 2), v = be16(f + 4);
      if (a >= HOLDING_COUNT) return broadcast ? void() : exception(fn, EX_ADDRESS);
      if (uint8_t ex = writeHolding(a, v)) return broadcast ? void() : exception(fn, ex);
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 15: {                                    // write multiple coils
      uint16_t a = be16(f + 2), q = be16(f + 4);
      uint8_t bc = f[6];
      if (q < 1 || q > 1968 || bc != (q + 7) / 8) return broadcast ? void() : exception(fn, EX_VALUE);
      if (a + q > RELAY_COUNT) return broadcast ? void() : exception(fn, EX_ADDRESS);
      if (a == 0 && q == RELAY_COUNT) {
        relays::setMask(f[7], relays::SRC_MODBUS);     // all eight in one go (interlock-aware)
      } else {
        for (uint16_t i = 0; i < q; i++)
          relays::set(a + i, (f[7 + i / 8] >> (i % 8)) & 1, relays::SRC_MODBUS);
      }
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 16: {                                    // write multiple registers
      uint16_t a = be16(f + 2), q = be16(f + 4);
      uint8_t bc = f[6];
      if (q < 1 || q > 123 || bc != q * 2) return broadcast ? void() : exception(fn, EX_VALUE);
      if (a + q > HOLDING_COUNT) return broadcast ? void() : exception(fn, EX_ADDRESS);
      for (uint16_t i = 0; i < q; i++)
        if (uint8_t ex = writeHolding(a + i, be16(f + 7 + i * 2))) return broadcast ? void() : exception(fn, ex);
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 8: {                                     // diagnostics, sub-function 0 = echo
      if (broadcast) return;
      if (be16(f + 2) != 0) return exception(fn, EX_FUNCTION);
      memcpy(out, f, n - 2);
      return send(out, n - 2);
    }
    default:
      if (!broadcast) exception(fn, EX_FUNCTION);
  }
}

// Bytes a request needs (0 = not known yet).
uint16_t expected() {
  if (len < 2) return 0;
  switch (buf[1]) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: return 8;
    case 15: case 16: return len >= 7 ? 9 + buf[6] : 0;
    default: return 0;
  }
}

void frame() {
  if (len < 4) { len = 0; return; }
  uint16_t c = crc16(buf, len - 2);
  if ((c & 0xFF) != buf[len - 2] || (c >> 8) != buf[len - 1]) {
    crcErr++;
    LOGD("modbus", "CRC error (%u bytes)", len);
    len = 0;
    discard = true;
    return;
  }
  bool broadcast = buf[0] == 0;
  good++;
  lastValid = millis() ? millis() : 1;
  leds::data();
  handle(buf, len, broadcast);
  len = 0;
}

void taskMain(void*) {
  health::watchThisTask();
  bool warnedAddr = false;
  while (running) {
    health::feed();
    addr = settings::modbusAddress();
    if (addr == 0 && !warnedAddr) {
      LOGE("modbus", "invalid slave address 0: set DIP AD1..AD7 (or a manual address)");
      warnedAddr = true;
    } else if (addr) {
      warnedAddr = false;
    }
    uint32_t now = micros();
    while (port.available()) {
      uint8_t b = port.read();
      now = micros();
      if (discard) { lastByteUs = now; continue; }
      if (len == 0 && b != addr && b != 0) { discard = true; lastByteUs = now; continue; }
      if (len < sizeof buf) buf[len++] = b;
      lastByteUs = now;
      uint16_t want = expected();
      if (want && len >= want) {
        if (addr) frame(); else len = 0;
        discard = true;                      // anything after a complete frame waits for silence
      }
    }
    if (now - lastByteUs >= silenceUs) {
      if (len && !discard && addr) frame();  // unknown function code: let CRC decide
      len = 0;
      discard = false;
    }
    vTaskDelay(1);
  }
  esp_task_wdt_delete(nullptr);
  task = nullptr;
  vTaskDelete(nullptr);
}

uint32_t serialConfig() {
  const Settings& s = settings::cur;
  bool two = s.mbStopBits == 2;
  switch (s.mbParity) {
    case 1: return two ? SERIAL_8E2 : SERIAL_8E1;
    case 2: return two ? SERIAL_8O2 : SERIAL_8O1;
    default: return two ? SERIAL_8N2 : SERIAL_8N1;
  }
}
}  // namespace

void begin() {
  const Settings& s = settings::cur;
  pinMode(PIN_RS485_SE, OUTPUT);
  digitalWrite(PIN_RS485_SE, LOW);              // receive
  int8_t rxPin = s.mbSwapRxTx ? PIN_RS485_TX : PIN_RS485_RX;
  int8_t txPin = s.mbSwapRxTx ? PIN_RS485_RX : PIN_RS485_TX;
  port.setRxBufferSize(512);
  port.begin(s.mbBaud, serialConfig(), rxPin, txPin);
  port.setRxFIFOFull(1);                         // every byte reaches us at once (gap timing)
  silenceUs = s.mbBaud <= 19200 ? 38500000UL / s.mbBaud : 1750;
  if (silenceUs < 2000) silenceUs = 2000;        // 1 ms task tick: keep margin
  addr = settings::modbusAddress();
  running = true;
  xTaskCreatePinnedToCore(taskMain, "modbus", 4096, nullptr, 3, &task, 1);
  const char* par[] = {"N", "E", "O"};
  LOGI("modbus", "RTU slave %u, %lu 8%s%u, RX GPIO%d TX GPIO%d", addr, (unsigned long)s.mbBaud,
       par[s.mbParity], s.mbStopBits, rxPin, txPin);
}

void stop() {
  running = false;
  while (task) vTaskDelay(5);
  port.end();
}

uint8_t address() { return addr; }
uint32_t lastFrameMs() { return lastValid; }
uint32_t goodFrames() { return good; }
uint32_t crcErrors() { return crcErr; }
}  // namespace modbus
