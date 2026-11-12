// =====================================================================================
//  modbus.cpp — Modbus RTU Slave روی RS-485
// =====================================================================================
//
//  برد در نقش Slave است؛ Master می‌تواند PLC، هاب مرکزی یا HMI باشد. Task جداگانه‌ای
//  فریم‌ها را دریافت و جواب می‌دهد تا کار رله‌ها و منو را معطل نکند.
//
//  توابع پشتیبانی‌شده: 1 و 2 (خواندن Coil و Discrete)، 3 و 4 (خواندن Holding و Input)،
//  5 و 6 (نوشتن یک Coil و یک رجیستر)، 15 و 16 (نوشتن چندتایی)، 8 (Echo). آدرس 0 = Broadcast.
//
//  ── نقشهٔ رجیسترها (شماره‌ها «داخلی» هستند؛ آدرس روی سیم = آدرس شروع جدول + شمارهٔ داخلی) ──
//   Coil     0..7     رله‌های ۱ تا ۸                                      (FC1، FC5، FC15)
//   Discrete 0..6     رزرو (۰)؛      7 = کلید کانفیگ                         (FC2)
//   Holding  0        ماسک رله‌ها (بیت ۰ = رله ۱)                            (FC3، FC6، FC16)
//            1        فرمان: 1 = همه خاموش، 2 = شناسایی (چراغ‌ها ۱۰ ثانیه چشمک)
//            2..9     پالس رله ۱ تا ۸ (مقدار × ۰٫۱ ثانیه؛ نوشتن، پالس را شروع می‌کند)
//            10..19   رزرو (خطای 02)
//            20..27   هر رله یک رجیستر: 0 = خاموش، 1 = روشن (برای PLC بدون دسترسی به Coil)
//   Input    0        ماسک رله‌ها در این لحظه                                (FC4)
//            1 کلید کانفیگ (بیت ۷)، 2 آدرس Slave، 3 نسخهٔ Firmware (major<<8 | minor)
//            4..5     زمان کارکرد (ثانیه، ۳۲ بیتی)
//            6, 7     فریم سالم، خطای CRC
//            8, 9     تعداد بوت، حالت ایمن
//            10..25   دفعات وصل شدن هر رله (۳۲ بیتی برای هر رله)
//
//  ── سازگاری با PLCهای مختلف ──
//   «آدرس شروع» هر جدول در منو قابل تنظیم است (Settings::mbBase). بعضی PLC یا HMI آدرس را از ۱
//   می‌شمارند یا فقط در بازهٔ مشخصی می‌خوانند؛ با آدرس شروع، نقشهٔ برد روی همان بازه می‌نشیند.
//   «حالت تحمل‌کننده» (mbLenient): وقتی PLC یک‌جا بلوکی را می‌خواند که فقط بخشی از آن در نقشه است،
//   بخش خارج از نقشه با صفر جواب داده می‌شود (به جای خطای 02). اگر کل درخواست خارج از نقشه باشد،
//   باز هم خطای 02 برمی‌گردد تا آدرس اشتباه دیده شود.
// =====================================================================================
#include <esp_task_wdt.h>
#include "core.h"

namespace modbus {
namespace {
// کدهای خطای Modbus
constexpr uint8_t EX_FUNCTION = 1;   // تابع پشتیبانی نمی‌شود
constexpr uint8_t EX_ADDRESS = 2;    // آدرس خارج از نقشه
constexpr uint8_t EX_VALUE = 3;      // مقدار یا تعداد نامعتبر

HardwareSerial& port = Serial2;      // UART2؛ پایه‌ها در config.h
TaskHandle_t task = nullptr;
volatile bool running = false;
uint8_t addr = 0;                    // آدرس Slave (0 = نامعتبر)
uint32_t silenceUs = 1750;           // سکوت پایان فریم (t3.5) به میکروثانیه
uint8_t buf[MB_FRAME_MAX];           // فریم در حال دریافت
uint16_t len = 0;
uint32_t lastByteUs = 0;
bool discard = false;                // تا سکوت خط، بایت‌ها را دور بریز (فریم برای Slave دیگر)
volatile uint32_t lastValid = 0;     // millis() آخرین فریم معتبر برای ما
uint32_t good = 0, crcErr = 0;       // فقط آمار

// ── ابزارهای پروتکل ────────────────────────────────────────────────────────────────

uint16_t crc16(const uint8_t* p, uint16_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= *p++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

uint16_t be16(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }

// ارسال جواب: CRC اضافه می‌شود، DE/RE روی ارسال می‌رود، پس از خروج آخرین بیت به دریافت برمی‌گردد.
void send(uint8_t* out, uint16_t n) {
  uint16_t c = crc16(out, n);
  out[n++] = c & 0xFF;
  out[n++] = c >> 8;
  if (settings::cur.mbRespDelayMs) vTaskDelay(pdMS_TO_TICKS(settings::cur.mbRespDelayMs));
  digitalWrite(PIN_RS485_SE, HIGH);
  port.write(out, n);
  port.flush();                                  // صبر می‌کند تا آخرین بیت واقعاً خارج شود
  digitalWrite(PIN_RS485_SE, LOW);
  while (port.available()) port.read();          // هر چه هنگام ارسال روی خط دیده شد (پژواک) دور ریخته شود
  leds::data();
}

void exception(uint8_t fn, uint8_t code) {
  uint8_t out[5] = {addr, (uint8_t)(fn | 0x80), code};
  send(out, 3);
}

// ── نگاشت آدرس روی سیم به شمارهٔ داخلی ─────────────────────────────────────────────────

uint32_t baseOf(uint8_t table) { return settings::cur.mbBase[table]; }

// درخواست [a, a+q) نسبت به جدول [base, base+طول) کجا قرار می‌گیرد؟
//   FIT_FULL    : کاملاً داخل نقشه (حالت عادی)
//   FIT_PARTIAL : بخشی داخل نقشه و حالت تحمل‌کننده روشن است (بخش خارج صفر خوانده یا نادیده گرفته می‌شود)
//   FIT_NONE    : هیچ اشتراکی با نقشه ندارد، یا بخشی خارج است و حالت تحمل‌کننده خاموش ← خطای 02
enum Fit : uint8_t { FIT_NONE, FIT_PARTIAL, FIT_FULL };

Fit fit(uint8_t table, uint16_t a, uint16_t q) {
  uint32_t lo = baseOf(table), hi = lo + MB_TABLE_LEN[table];
  uint32_t s = a, e = s + q;
  if (s >= lo && e <= hi) return FIT_FULL;
  if (e <= lo || s >= hi) return FIT_NONE;
  return settings::cur.mbLenient ? FIT_PARTIAL : FIT_NONE;
}

// شمارهٔ داخلی یک آدرس روی سیم؛ -1 اگر خارج از نقشه
int idxOf(uint8_t table, uint32_t wire) {
  uint32_t lo = baseOf(table);
  if (wire < lo || wire >= lo + MB_TABLE_LEN[table]) return -1;
  return (int)(wire - lo);
}

// ── مقدارهای جدول‌ها (شمارهٔ داخلی) ───────────────────────────────────────────────────

bool readBit(uint8_t table, uint16_t idx) {
  if (table == MB_T_COIL) return (relays::state() >> idx) & 1;
  return (inputs::raw() >> idx) & 1;               // Discrete: کلید کانفیگ (بیت ۷)
}

uint16_t holding(uint16_t idx) {
  if (idx == 0) return relays::state();
  if (idx >= MB_RELAY_REGS_START && idx < MB_RELAY_REGS_START + RELAY_COUNT)
    return (relays::state() >> (idx - MB_RELAY_REGS_START)) & 1;
  return 0;                                        // فرمان‌ها و پالس‌ها خوانده نمی‌شوند (فقط نوشتنی)
}

// یک مقدار ۳۲ بیتی در دو رجیستر: پیش‌فرض اول کلمهٔ بالا؛ با mbWordSwap اول کلمهٔ پایین.
uint16_t word32(uint32_t v, bool first) {
  bool high = first != settings::cur.mbWordSwap;
  return high ? v >> 16 : v & 0xFFFF;
}

uint16_t inputReg(uint16_t idx) {
  uint32_t up = millis() / 1000;
  switch (idx) {
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
  if (idx >= 10 && idx < 26) {                     // دفعات وصل شدن رله‌ها: هر رله دو رجیستر
    uint32_t c = relays::cycles((idx - 10) / 2);
    return word32(c, ((idx - 10) & 1) == 0);
  }
  return 0;
}

// نوشتن یک رجیستر Holding (شمارهٔ داخلی). 0 = انجام شد، وگرنه کد خطای Modbus.
uint8_t writeHolding(uint16_t idx, uint16_t v) {
  if (idx == 0) {                                  // ماسک همه رله‌ها
    if (v > 0xFF) return EX_VALUE;
    relays::setMask(v, relays::SRC_MODBUS);
  } else if (idx == 1) {                           // فرمان
    if (v == 1) relays::allOff(relays::SRC_MODBUS, false);
    else if (v == 2) leds::identify(10000);
    else if (v != 0) return EX_VALUE;
  } else if (idx >= 2 && idx < 2 + RELAY_COUNT) {  // پالس: مقدار × ۰٫۱ ثانیه (صفر = خاموش)
    uint8_t r = idx - 2;
    if (v) relays::set(r, true, relays::SRC_MODBUS, (uint32_t)v * 100);
    else relays::set(r, false, relays::SRC_MODBUS);
  } else if (idx >= MB_RELAY_REGS_START && idx < MB_RELAY_REGS_START + RELAY_COUNT) {
    if (v > 1) return EX_VALUE;                    // هر رله یک رجیستر: فقط 0 یا 1
    relays::set(idx - MB_RELAY_REGS_START, v == 1, relays::SRC_MODBUS);
  } else {
    return EX_ADDRESS;                             // 10..19 رزرو
  }
  return 0;
}

// ── اجرای یک درخواست ────────────────────────────────────────────────────────────────────
// f = فریم کامل بدون CRC معتبر شده، broadcast = آدرس 0 (بدون جواب).
void handle(const uint8_t* f, uint16_t n, bool broadcast) {
  uint8_t fn = f[1];
  uint8_t out[MB_FRAME_MAX];
  out[0] = addr;
  out[1] = fn;
  bool lenient = settings::cur.mbLenient;
  switch (fn) {
    case 1:
    case 2: {                                      // خواندن Coil / Discrete
      uint8_t t = fn == 1 ? MB_T_COIL : MB_T_DISCRETE;
      uint16_t a = be16(f + 2), q = be16(f + 4);
      if (broadcast) return;
      if (q < 1 || q > 2000) return exception(fn, EX_VALUE);
      if (fit(t, a, q) == FIT_NONE) return exception(fn, EX_ADDRESS);
      uint8_t bytes = (q + 7) / 8;
      out[2] = bytes;
      memset(out + 3, 0, bytes);
      for (uint16_t i = 0; i < q; i++) {
        int idx = idxOf(t, (uint32_t)a + i);       // خارج از نقشه (فقط حالت تحمل‌کننده) = 0
        if (idx >= 0 && readBit(t, idx)) out[3 + i / 8] |= 1 << (i % 8);
      }
      return send(out, 3 + bytes);
    }
    case 3:
    case 4: {                                      // خواندن Holding / Input
      uint8_t t = fn == 3 ? MB_T_HOLDING : MB_T_INPUT;
      uint16_t a = be16(f + 2), q = be16(f + 4);
      if (broadcast) return;
      if (q < 1 || q > 125) return exception(fn, EX_VALUE);
      if (fit(t, a, q) == FIT_NONE) return exception(fn, EX_ADDRESS);
      out[2] = q * 2;
      for (uint16_t i = 0; i < q; i++) {
        int idx = idxOf(t, (uint32_t)a + i);
        uint16_t v = idx < 0 ? 0 : (fn == 3 ? holding(idx) : inputReg(idx));
        out[3 + i * 2] = v >> 8;
        out[4 + i * 2] = v & 0xFF;
      }
      return send(out, 3 + q * 2);
    }
    case 5: {                                      // نوشتن یک Coil (رله)
      uint16_t a = be16(f + 2), v = be16(f + 4);
      if (v != 0xFF00 && v != 0x0000) return broadcast ? void() : exception(fn, EX_VALUE);
      int idx = idxOf(MB_T_COIL, a);
      if (idx < 0) return broadcast ? void() : exception(fn, EX_ADDRESS);
      relays::set(idx, v == 0xFF00, relays::SRC_MODBUS);
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 6: {                                      // نوشتن یک رجیستر Holding
      uint16_t a = be16(f + 2), v = be16(f + 4);
      int idx = idxOf(MB_T_HOLDING, a);
      if (idx < 0) return broadcast ? void() : exception(fn, EX_ADDRESS);
      if (uint8_t ex = writeHolding(idx, v)) return broadcast ? void() : exception(fn, ex);
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 15: {                                     // نوشتن چند Coil
      uint16_t a = be16(f + 2), q = be16(f + 4);
      uint8_t bc = f[6];
      if (q < 1 || q > 1968 || bc != (q + 7) / 8) return broadcast ? void() : exception(fn, EX_VALUE);
      if (fit(MB_T_COIL, a, q) == FIT_NONE) return broadcast ? void() : exception(fn, EX_ADDRESS);
      if (a == baseOf(MB_T_COIL) && q == RELAY_COUNT) {
        relays::setMask(f[7], relays::SRC_MODBUS);   // هر ۸ رله یک‌جا (با رعایت اینترلاک)
      } else {
        for (uint16_t i = 0; i < q; i++) {
          int idx = idxOf(MB_T_COIL, (uint32_t)a + i);
          if (idx >= 0) relays::set(idx, (f[7 + i / 8] >> (i % 8)) & 1, relays::SRC_MODBUS);
        }
      }
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 16: {                                     // نوشتن چند رجیستر Holding
      uint16_t a = be16(f + 2), q = be16(f + 4);
      uint8_t bc = f[6];
      if (q < 1 || q > 123 || bc != q * 2) return broadcast ? void() : exception(fn, EX_VALUE);
      if (fit(MB_T_HOLDING, a, q) == FIT_NONE) return broadcast ? void() : exception(fn, EX_ADDRESS);
      for (uint16_t i = 0; i < q; i++) {
        int idx = idxOf(MB_T_HOLDING, (uint32_t)a + i);
        if (idx < 0) continue;                     // بخش خارج از نقشه (فقط حالت تحمل‌کننده): نادیده
        uint8_t ex = writeHolding(idx, be16(f + 7 + i * 2));
        if (ex == EX_ADDRESS && lenient) continue; // رجیسترهای رزرو در بلوک بزرگ: نادیده
        if (ex) return broadcast ? void() : exception(fn, ex);
      }
      if (broadcast) return;
      memcpy(out, f, 6);
      return send(out, 6);
    }
    case 8: {                                      // تشخیصی: زیرتابع 0 = Echo
      if (broadcast) return;
      if (be16(f + 2) != 0) return exception(fn, EX_FUNCTION);
      memcpy(out, f, n - 2);
      return send(out, n - 2);
    }
    default:
      if (!broadcast) exception(fn, EX_FUNCTION);
  }
}

// ── دریافت فریم ────────────────────────────────────────────────────────────────────────

// طول مورد نیاز درخواست با توجه به تابع؛ 0 = هنوز معلوم نیست
uint16_t expected() {
  if (len < 2) return 0;
  switch (buf[1]) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: return 8;
    case 15: case 16: return len >= 7 ? 9 + buf[6] : 0;
    default: return 0;
  }
}

// فریم کامل شد: CRC بررسی و اجرا می‌شود
void frame() {
  if (len < 4) { len = 0; return; }
  uint16_t c = crc16(buf, len - 2);
  if ((c & 0xFF) != buf[len - 2] || (c >> 8) != buf[len - 1]) {
    crcErr++;
    LOGD("modbus", "CRC error (%u bytes)", len);
    len = 0;
    discard = true;                                // تا سکوت خط ادامهٔ فریم خراب را نخوان
    return;
  }
  bool broadcast = buf[0] == 0;
  good++;
  lastValid = millis() ? millis() : 1;
  leds::data();
  handle(buf, len, broadcast);
  len = 0;
}

// Task اصلی مدباس: بایت‌ها را جمع می‌کند و با سکوت t3.5 یا رسیدن طول مورد انتظار، فریم را کامل می‌داند.
void taskMain(void*) {
  health::watchThisTask();
  bool warnedAddr = false;
  while (running) {
    health::feed();
    addr = settings::modbusAddress();              // آدرس منو ممکن است عوض شده باشد
    if (addr == 0 && !warnedAddr) {
      LOGE("modbus", "invalid slave address: set the Modbus address (1..247) in the config menu");
      warnedAddr = true;
    } else if (addr) {
      warnedAddr = false;
    }
    uint32_t now = micros();
    while (port.available()) {
      uint8_t b = port.read();
      now = micros();
      if (discard) { lastByteUs = now; continue; }
      if (len == 0 && b != addr && b != 0) { discard = true; lastByteUs = now; continue; }  // فریم Slave دیگر
      if (len < sizeof buf) buf[len++] = b;
      lastByteUs = now;
      uint16_t want = expected();
      if (want && len >= want) {
        if (addr) frame(); else len = 0;
        discard = true;                            // هر چه بعد از فریم کامل آمد تا سکوت نادیده
      }
    }
    if (now - lastByteUs >= silenceUs) {
      if (len && !discard && addr) frame();        // تابع ناشناخته: CRC تصمیم می‌گیرد
      len = 0;
      discard = false;
    }
    vTaskDelay(1);
  }
  esp_task_wdt_delete(nullptr);
  task = nullptr;
  vTaskDelete(nullptr);
}

// قالب سریال (۸ بیت داده + Parity + Stop) از تنظیمات
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
  digitalWrite(PIN_RS485_SE, LOW);                 // پیش‌فرض: حالت دریافت
  int8_t rxPin = s.mbSwapRxTx ? PIN_RS485_TX : PIN_RS485_RX;
  int8_t txPin = s.mbSwapRxTx ? PIN_RS485_RX : PIN_RS485_TX;
  port.setRxBufferSize(MB_UART_RX_BUFFER);
  port.begin(s.mbBaud, serialConfig(), rxPin, txPin);
  port.setRxFIFOFull(1);                           // هر بایت بلافاصله به برنامه برسد (برای اندازه‌گیری فاصلهٔ فریم)
  // سکوت پایان فریم = ۳٫۵ کاراکتر (۱۱ بیت) برای سرعت‌های تا ۱۹۲۰۰؛ بالاتر از آن مقدار ثابت ۱۷۵۰µs
  silenceUs = s.mbBaud <= 19200 ? 38500000UL / s.mbBaud : 1750;
  if (silenceUs < 2000) silenceUs = 2000;          // تیک Task یک میلی‌ثانیه است: کمی حاشیه
  addr = settings::modbusAddress();
  running = true;
  xTaskCreatePinnedToCore(taskMain, "modbus", MB_TASK_STACK, nullptr, MB_TASK_PRIORITY, &task, 1);
  const char* par[] = {"N", "E", "O"};
  LOGI("modbus", "RTU slave %u, %lu 8%s%u, RX GPIO%d TX GPIO%d, bases C%u D%u I%u H%u%s", addr,
       (unsigned long)s.mbBaud, par[s.mbParity], s.mbStopBits, rxPin, txPin, s.mbBase[MB_T_COIL],
       s.mbBase[MB_T_DISCRETE], s.mbBase[MB_T_INPUT], s.mbBase[MB_T_HOLDING],
       s.mbLenient ? ", lenient" : "");
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
