// =====================================================================================
//  relays.cpp — رله‌ها
// =====================================================================================
//
//  همهٔ تغییرهای رله، از هر منبع (منو، MQTT، Modbus، کنترل محلی، قواعد آفلاین، قطع ارتباط)
//  و از هر Task، فقط از طریق post() وارد یک صف می‌شود. حلقهٔ اصلی (loop) صف را خالی و اجرا می‌کند:
//    - حداقل فاصلهٔ دو تغییر هر رله (حفاظت کنتاکت)      → Settings::minIntervalMs
//    - فاصلهٔ وصل شدن دو کویل (کم‌کردن جریان هجومی)    → Settings::staggerMs
//    - اینترلاک: دو رلهٔ شریک هیچ‌وقت هم‌زمان وصل نمی‌شوند (اول رلهٔ شریک قطع می‌شود)
//    - پالس: رله بعد از مدت مشخص خودش خاموش می‌شود (درب‌بازکن، زنگ)
//    - ذخیرهٔ وضعیت در NVS برای بازگشت پس از قطع برق (با محافظت از فرسایش فلش؛ config.h)
//  فرمان‌هایی که شناسه (id) دارند، پس از رسیدن رله به وضعیت هدف «انجام شد» و در غیر این صورت
//  «ناموفق» (اینترلاک، رلهٔ نصب‌نشده، جایگزین شدن با فرمان تازه‌تر) به سرور گزارش می‌شوند.
// =====================================================================================
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "core.h"

namespace relays {
namespace {
QueueHandle_t queue = nullptr;
ResultFn resultFn = nullptr;

volatile uint8_t cur = 0;         // energised now
uint8_t want = 0;                 // target
uint8_t forced = 0;               // pending changes that skip the minimum interval
uint8_t persisted = 0;
bool noPersist = false;   // حالت تنظیمات: تست منو وضعیت ذخیره‌شده را عوض نمی‌کند
uint32_t lastChange[RELAY_COUNT] = {0};
uint32_t pulseUntil[RELAY_COUNT] = {0};
uint32_t cycleCount[RELAY_COUNT] = {0};
bool cyclesDirty = false;
uint32_t cyclesSavedAt = 0;
uint32_t lastOnAt = 0;
uint32_t lastAnyChange = 0;
uint32_t lastPersist = 0;
uint32_t changeCount = 0;
char pendingId[RELAY_COUNT][40] = {{0}};   // command waiting for its relay to reach the target
Preferences prefs;
bool prefsOk = false;

void report(const char* id, bool ok, uint8_t r, bool v, const char* err) {
  if (id && *id && resultFn) resultFn(id, ok, r, v, err);
}

void drive(uint8_t r, bool on) {
  digitalWrite(RELAY_PINS[r], on ? HIGH : LOW);
  if (on) {
    cur |= 1 << r;
    cycleCount[r]++;
    cyclesDirty = true;
    lastOnAt = millis();
  } else {
    cur &= ~(1 << r);
  }
  lastChange[r] = millis();
  lastAnyChange = lastChange[r];
  changeCount++;
}

// ذخیرهٔ وضعیت رله‌ها و شمارندهٔ دفعات وصل شدن در NVS، با رعایت محدودیت‌های فرسایش فلش.
void persist() {
  // رله‌ای که در حال پالس است «خاموش» ذخیره می‌شود: پس از قطع برق نباید روشن بماند.
  uint8_t v = cur & ~pulsing();
  uint32_t now = millis();
  if (!noPersist && v != persisted && prefsOk && now - lastAnyChange >= RELAY_STATE_STABLE_MS &&
      (lastPersist == 0 || now - lastPersist >= RELAY_STATE_MIN_GAP_MS)) {
    prefs.putUChar("st", v);
    persisted = v;
    lastPersist = now ? now : 1;
  }
  if (cyclesDirty && prefsOk && millis() - cyclesSavedAt >= RELAY_CYCLES_SAVE_MS) {
    prefs.putBytes("cyc", cycleCount, sizeof cycleCount);
    cyclesDirty = false;
    cyclesSavedAt = millis();
  }
}

// اینترلاک: رلهٔ شریک باید (هم اکنون و هم در وضعیت هدف) خاموش باشد تا این رله وصل شود.
bool interlocked(uint8_t r, uint8_t targetMask) {
  uint8_t p = settings::cur.interlock[r];
  if (!p) return false;
  uint8_t bit = 1 << (p - 1);
  return (cur & bit) || (targetMask & bit);
}

void supersede(uint8_t r) {
  if (pendingId[r][0]) {
    report(pendingId[r], false, r, (cur >> r) & 1, "superseded");
    pendingId[r][0] = 0;
  }
}

// اجرای یک فرمان از صف: فقط «وضعیت هدف» (want) را عوض می‌کند. وصل و قطع واقعی را apply()
// با رعایت زمان‌بندی انجام می‌دهد.
void handle(const Cmd& c) {
  switch (c.op) {
    case OP_SET: {
      uint8_t r = c.relay;
      if (r >= RELAY_COUNT || !fitted(r)) {
        report(c.id, false, r, false, "not_fitted");
        LOGW("relay", "%s: relay %u is not fitted", sourceName(c.src), r + 1);
        return;
      }
      if (c.value && interlocked(r, want & ~(1 << r))) {
        report(c.id, false, r, (cur >> r) & 1, "interlock");
        LOGW("relay", "%s: relay %u refused, interlocked with relay %u", sourceName(c.src), r + 1,
             settings::cur.interlock[r]);
        return;
      }
      supersede(r);
      if (c.value) want |= 1 << r; else want &= ~(1 << r);
      if (c.force) forced |= 1 << r;
      pulseUntil[r] = (c.value && c.pulseMs) ? millis() + c.pulseMs : 0;
      if (pulseUntil[r] == 0 && c.value && c.pulseMs) pulseUntil[r] = 1;   // millis() wrapped to 0
      if (c.id[0]) strlcpy(pendingId[r], c.id, sizeof pendingId[r]);
      LOGI("relay", "%s: relay %u -> %s%s", sourceName(c.src), r + 1, c.value ? "ON" : "OFF",
           c.pulseMs && c.value ? " (pulse)" : "");
      break;
    }
    case OP_MASK: {
      uint8_t m = c.mask & fittedMask();
      for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        uint8_t p = settings::cur.interlock[r];
        if ((m & (1 << r)) && p && (m & (1 << (p - 1)))) {
          LOGW("relay", "%s: relays %u and %u are interlocked, both left off", sourceName(c.src),
               r + 1, p);
          m &= ~((1 << r) | (1 << (p - 1)));
        }
      }
      for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        if (((want >> r) & 1) != ((m >> r) & 1)) supersede(r);
        pulseUntil[r] = 0;
      }
      // Partners that must go off first: their ON partner waits (see loop) until they are off.
      want = m;
      if (c.force) forced = 0xFF;
      LOGI("relay", "%s: outputs -> 0x%02X", sourceName(c.src), m);
      break;
    }
    case OP_ALL_OFF:
      for (uint8_t r = 0; r < RELAY_COUNT; r++) { supersede(r); pulseUntil[r] = 0; }
      want = 0;
      if (c.force) forced = 0xFF;
      LOGI("relay", "%s: all outputs OFF", sourceName(c.src));
      break;
  }
}

// وضعیت واقعی رله‌ها را به وضعیت هدف می‌رساند.
// اول قطع‌ها انجام می‌شود (تا رلهٔ شریک اینترلاک آزاد شود) و بعد وصل‌ها، هر بار یک کویل.
void apply() {
  uint32_t now = millis();
  const Settings& s = settings::cur;
  // OFF first, so an interlocked partner is released before the other one closes.
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    uint8_t bit = 1 << r;
    if (!(cur & bit) || (want & bit) || !fitted(r)) continue;
    if (!(forced & bit) && now - lastChange[r] < s.minIntervalMs) continue;
    drive(r, false);
    forced &= ~bit;
  }
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    uint8_t bit = 1 << r;
    if ((cur & bit) || !(want & bit) || !fitted(r)) continue;
    if (!(forced & bit) && now - lastChange[r] < s.minIntervalMs) continue;
    if (now - lastOnAt < s.staggerMs) break;            // one coil at a time (inrush)
    uint8_t p = s.interlock[r];
    if (p && (cur & (1 << (p - 1)))) continue;          // partner still releasing
    drive(r, true);
    forced &= ~bit;
  }
  // Report commands whose relay reached its target.
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    if (!pendingId[r][0]) continue;
    bool target = (want >> r) & 1;
    if (((cur >> r) & 1) == target) {
      report(pendingId[r], true, r, target, nullptr);
      pendingId[r][0] = 0;
    }
  }
}
}  // namespace

// راه‌اندازی: پایه‌ها ابتدا LOW می‌شوند (پیش از OUTPUT شدن) تا رله‌ها لحظه‌ای هم نپرند.
// وضعیت اولیه از حالت «پس از وصل برق» هر رله و آخرین وضعیت ذخیره‌شده می‌آید؛ در حالت ایمن همه خاموش.
void begin(bool safeMode, bool configMode) {
  queue = xQueueCreate(RELAY_QUEUE_LEN, sizeof(Cmd));
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    if (!fitted(r)) continue;
    digitalWrite(RELAY_PINS[r], LOW);
    pinMode(RELAY_PINS[r], OUTPUT);
  }
  prefsOk = prefs.begin("rt", false);
  uint8_t last = prefsOk ? prefs.getUChar("st", 0) : 0;
  if (prefsOk && prefs.getBytesLength("cyc") == sizeof cycleCount)
    prefs.getBytes("cyc", cycleCount, sizeof cycleCount);
  persisted = last;
  uint8_t m = 0;
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    uint8_t mode = settings::cur.powerOn[r];
    bool on = mode == PON_ON || (mode == PON_RESTORE && (last >> r) & 1);
    if (on) m |= 1 << r;
  }
  if (safeMode) {
    LOGW("relay", "safe mode: all outputs stay OFF (stored 0x%02X)", last);
    m = 0;
  }
  // حالت تنظیمات (منو): همهٔ خروجی‌ها خاموش، و آنچه در منوی تست روشن و خاموش می‌شود ذخیره
  // نمی‌شود؛ پس از خروج از منو، رله‌ها طبق «حالت پس از وصل برق» و وضعیت قبلی برمی‌گردند.
  if (configMode) {
    LOGI("relay", "config mode: all outputs OFF, the stored state 0x%02X is kept", last);
    m = 0;
    noPersist = true;
  }
  m &= fittedMask();
  // Applied by loop() with the stagger, so the coils do not all pull in at once.
  Cmd c = {};
  c.op = OP_MASK;
  c.mask = m;
  c.src = SRC_BOOT;
  c.force = true;
  handle(c);
  LOGI("relay", "power-on outputs 0x%02X (stored 0x%02X)", m, last);
}

void loop() {
  Cmd c;
  for (uint8_t n = 0; n < RELAY_CMDS_PER_LOOP && xQueueReceive(queue, &c, 0) == pdTRUE; n++) handle(c);
  uint32_t now = millis();
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    if (pulseUntil[r] && (cur & (1 << r)) && (int32_t)(now - pulseUntil[r]) >= 0) {
      pulseUntil[r] = 0;
      want &= ~(1 << r);
      LOGI("relay", "pulse: relay %u -> OFF", r + 1);
    }
  }
  apply();
  persist();
}

bool post(const Cmd& c) {
  if (queue == nullptr) return false;
  if (xQueueSend(queue, &c, 0) != pdTRUE) {
    LOGW("relay", "command queue full, %s command dropped", sourceName(c.src));
    return false;
  }
  return true;
}

bool set(uint8_t relay, bool on, Source src, uint32_t pulseMs) {
  Cmd c = {};
  c.op = OP_SET;
  c.relay = relay;
  c.value = on;
  c.src = src;
  c.pulseMs = pulseMs;
  return post(c);
}

bool setMask(uint8_t mask, Source src) {
  Cmd c = {};
  c.op = OP_MASK;
  c.mask = mask;
  c.src = src;
  return post(c);
}

bool allOff(Source src, bool force) {
  Cmd c = {};
  c.op = OP_ALL_OFF;
  c.src = src;
  c.force = force;
  return post(c);
}

void onResult(ResultFn fn) { resultFn = fn; }
uint8_t state() { return cur; }

uint8_t pulsing() {
  uint8_t m = 0;
  for (uint8_t r = 0; r < RELAY_COUNT; r++) if (pulseUntil[r]) m |= 1 << r;
  return m;
}

bool fitted(uint8_t relay) { return relay < RELAY_COUNT && RELAY_PINS[relay] >= 0; }

uint8_t fittedMask() {
  uint8_t m = 0;
  for (uint8_t r = 0; r < RELAY_COUNT; r++) if (fitted(r)) m |= 1 << r;
  return m;
}

uint32_t cycles(uint8_t relay) { return relay < RELAY_COUNT ? cycleCount[relay] : 0; }
uint32_t changes() { return changeCount; }

const char* sourceName(uint8_t src) {
  static const char* names[] = {"boot", "web", "mqtt", "modbus", "link-loss", "system", "local", "rule"};
  return src < sizeof names / sizeof names[0] ? names[src] : "?";
}

void saveNow() {
  if (!prefsOk) return;
  prefs.putUChar("st", cur & ~pulsing());
  persisted = cur & ~pulsing();
  prefs.putBytes("cyc", cycleCount, sizeof cycleCount);
  cyclesDirty = false;
}
}  // namespace relays
