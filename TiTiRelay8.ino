// =====================================================================================
//  TiTi Relay-8 — ماژول خروجی ۸ کانال رله (ESP32-WROOM-32) برای پلتفرم TiTiSwitch
// =====================================================================================
//
//  این فایل «بدنهٔ اصلی» برنامه است و بخش‌های کوچک و پایه هم داخل همین فایل جمع شده‌اند:
//    ۱) گزارش سریال (logx)        ۲) سلامت و Watchdog (health)     ۳) ورودی‌ها/دیپ‌سوئیچ (inputs)
//    ۴) چراغ‌ها (leds)            ۵) کنسول خط تولید (console)      ۶) setup() و loop()
//  بخش‌های بزرگ‌تر در فایل‌های جدا هستند (فهرست در core.h). همهٔ تنظیمات و پایه‌ها در config.h.
//
//  حالت‌های کار (در لحظهٔ روشن شدن تعیین می‌شود):
//    کلید کانفیگ (دیپ ۸) روشن   ← منوی تنظیمات؛ وای‌فای «TiTi-R8-xxxx»، نشانی 192.168.1.100
//    کلید کانفیگ خاموش          ← ارتباطی که در منو انتخاب شده:
//        Wi-Fi  : MQTT به پلتفرم (قرارداد native) یا یک بروکر عمومی
//        RS-485 : Modbus RTU Slave؛ آدرس از منو (دیپ AD1..AD7 ورودی آزاد است)
//    اگر هنوز هیچ ارتباطی انتخاب نشده، حتی با کلید خاموش منوی تنظیمات باز می‌شود.
//
//  راه‌اندازی در Arduino IDE: برد «ESP32 Dev Module»، Partition Scheme «Minimal SPIFFS
//  (1.9MB APP with OTA)». کتابخانه‌ها: ArduinoJson 7، ESP Async WebServer 3، Async TCP 3.
//  جزئیات در README.md.
// =====================================================================================

#include <Preferences.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core.h"

// =====================================================================================
//  ۱) گزارش سریال (logx)
//  - چند سطح (خطا، هشدار، اطلاعات، اشکال‌زدایی)
//  - پیام تکراری (همان تگ و همان قالب) در بازهٔ LOG_REPEAT_WINDOW_MS خلاصه می‌شود تا سریال شلوغ نشود
//  - چند خط آخر در حافظه می‌ماند و منوی وب آن را زنده نشان می‌دهد
//  - از چند Task هم‌زمان صدا زده می‌شود، پس با یک قفل (mutex) محافظت شده است
// =====================================================================================
namespace logx {
namespace {
const char LVL_CHAR[] = {'E', 'W', 'I', 'D'};

SemaphoreHandle_t mtx = nullptr;
uint8_t curLevel = DEFAULT_LOG_LEVEL;
Entry ring[RING];          // حلقهٔ چند خط آخر
uint8_t head = 0;
uint32_t seq = 0;          // شمارهٔ ترتیبی هر خط (منو با آن فقط خط‌های تازه را می‌گیرد)

// تشخیص پیام تکراری با کلید (تگ، رشتهٔ قالب)؛ پس «تلاش بعدی ۱۲ ثانیه دیگر» و
// «تلاش بعدی ۲۴ ثانیه دیگر» هم یک پیام حساب می‌شوند.
const char* lastTag = nullptr;
const char* lastFmt = nullptr;
uint32_t lastMs = 0;
uint32_t repeats = 0;

void emit(LogLevel lvl, const char* tag, const char* msg) {
  char line[LINE + 24];
  uint32_t ms = millis();
  snprintf(line, sizeof line, "[%6lu.%03lu] %c %-6s %s", (unsigned long)(ms / 1000),
           (unsigned long)(ms % 1000), LVL_CHAR[lvl], tag, msg);
  Serial.println(line);
  Entry& e = ring[head];
  e.seq = ++seq;
  e.ms = ms;
  e.lvl = lvl;
  snprintf(e.text, LINE, "%s: %s", tag, msg);
  head = (head + 1) % RING;
}

void emitRepeats() {
  if (repeats == 0) return;
  char msg[48];
  snprintf(msg, sizeof msg, "(previous message repeated %lu more times)", (unsigned long)repeats);
  emit(LOG_INFO, lastTag ? lastTag : "log", msg);
  repeats = 0;
}
}  // namespace

void begin(uint32_t baud) {
  Serial.begin(baud);
  mtx = xSemaphoreCreateMutex();
}

void setLevel(uint8_t level) { curLevel = level > LOG_DEBUG ? LOG_DEBUG : level; }
uint8_t level() { return curLevel; }

void write(LogLevel lvl, const char* tag, const char* fmt, ...) {
  if (lvl > curLevel || mtx == nullptr) return;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;   // اگر قفل آزاد نشد، پیام رد می‌شود (برنامه معطل نمی‌ماند)
  uint32_t now = millis();
  if (tag == lastTag && fmt == lastFmt && now - lastMs < LOG_REPEAT_WINDOW_MS) {
    repeats++;
    lastMs = now;
    xSemaphoreGive(mtx);
    return;
  }
  emitRepeats();
  char msg[LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  emit(lvl, tag, msg);
  lastTag = tag;
  lastFmt = fmt;
  lastMs = now;
  xSemaphoreGive(mtx);
}

void flushRepeats() {
  if (repeats == 0 || millis() - lastMs < LOG_REPEAT_WINDOW_MS) return;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
  emitRepeats();
  lastFmt = nullptr;
  xSemaphoreGive(mtx);
}

uint8_t since(uint32_t after, Entry* out, uint8_t max) {
  uint8_t n = 0;
  if (mtx == nullptr || xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  for (uint8_t i = 0; i < RING && n < max; i++) {
    const Entry& e = ring[(head + i) % RING];   // از قدیمی‌ترین شروع می‌کنیم
    if (e.seq > after) out[n++] = e;
  }
  xSemaphoreGive(mtx);
  return n;
}

uint32_t lastSeq() { return seq; }
}  // namespace logx

// =====================================================================================
//  ۲) سلامت سیستم (health)
//  - Watchdog: اگر حلقهٔ اصلی یا Task مدباس گیر کند، برد خودش ریست می‌شود
//  - علت هر ریست ثبت می‌شود؛ سه ریست غیرعادی (کرش، Watchdog، افت ولتاژ) پشت سر هم = «حالت ایمن»:
//    رله‌ها خاموش بالا می‌آیند تا برد معیوب بار را مدام روشن/خاموش نکند
//  - اگر حافظهٔ آزاد مدتی کم بماند، ریست کنترل‌شده انجام می‌شود (نشتی حافظه)
// =====================================================================================
namespace health {
namespace {
bool safe = false;
bool stableMarked = false;
uint32_t boots = 0;
uint8_t badBoots = 0;
uint32_t lowHeapSince = 0;
uint32_t lastHeapReport = 0;
esp_reset_reason_t reason = ESP_RST_UNKNOWN;

// ریست‌هایی که نشانهٔ خرابی هستند (ریست عادی، قطع و وصل برق یا دکمه جزو آن نیست)
bool abnormal(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}
}  // namespace

void begin() {
  reason = esp_reset_reason();
  esp_task_wdt_config_t wcfg = {};
  wcfg.timeout_ms = HEALTH_WDT_TIMEOUT_MS;
  wcfg.idle_core_mask = 0;
  wcfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wcfg) != ESP_OK) esp_task_wdt_init(&wcfg);

  // شمارندهٔ بوت و شمارندهٔ ریست‌های غیرعادی پشت سر هم (در NVS)
  Preferences p;
  if (p.begin("hl", false)) {
    boots = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", boots);
    badBoots = abnormal(reason) ? p.getUChar("bad", 0) + 1 : 0;
    p.putUChar("bad", badBoots);
    p.end();
  }
  safe = badBoots >= HEALTH_CRASH_LIMIT;
  if (abnormal(reason)) LOGW("health", "abnormal reset: %s (%u in a row)", resetReason(), badBoots);
  if (safe) LOGE("health", "SAFE MODE: %u abnormal resets in a row", badBoots);
}

void watchThisTask() { esp_task_wdt_add(nullptr); }
void feed() { esp_task_wdt_reset(); }

void loop() {
  uint32_t now = millis();
  // پس از HEALTH_STABLE_AFTER_MS کار بدون خطا، شمارندهٔ ریست‌های غیرعادی صفر می‌شود
  if (!stableMarked && now > HEALTH_STABLE_AFTER_MS) {
    stableMarked = true;
    if (badBoots) {
      Preferences p;
      if (p.begin("hl", false)) { p.putUChar("bad", 0); p.end(); }
    }
  }
  uint32_t heap = ESP.getFreeHeap();
  if (heap < HEALTH_LOW_HEAP_BYTES) {
    if (!lowHeapSince) {
      lowHeapSince = now ? now : 1;
      LOGW("health", "free heap low: %lu bytes", (unsigned long)heap);
    } else if (now - lowHeapSince > HEALTH_LOW_HEAP_FOR_MS) {
      restart("free heap stayed low");
    }
  } else {
    lowHeapSince = 0;
  }
  if (now - lastHeapReport >= HEALTH_HEAP_REPORT_MS) {
    lastHeapReport = now;
    LOGD("health", "heap free %lu, min %lu, largest %lu", (unsigned long)heap,
         (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  }
}

bool lowHeap() { return ESP.getFreeHeap() < HEALTH_LOW_HEAP_BYTES; }
bool safeMode() { return safe; }
uint32_t bootCount() { return boots; }

const char* resetReason() {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void restart(const char* why) {
  LOGW("health", "restart: %s", why);
  relays::saveNow();          // وضعیت رله‌ها پیش از ریست در حافظه نوشته شود
  Serial.flush();
  delay(200);
  ESP.restart();
}
}  // namespace health

// =====================================================================================
//  ۳) ورودی‌ها (inputs): دیپ‌سوئیچ آدرس و کلید کانفیگ
//  هر ۱۰ میلی‌ثانیه خوانده می‌شود و تغییر باید ۵۰ میلی‌ثانیه ثابت بماند تا پذیرفته شود
//  (حذف لرزش کلید). هنگام روشن شدن چند بار خوانده و رأی اکثریت گرفته می‌شود تا کلیدی که
//  لحظهٔ روشن شدن می‌لرزد، حالت کار را اشتباه تعیین نکند.
// =====================================================================================
namespace inputs {
namespace {
volatile uint8_t stable = 0;    // مقدار پایدار: بیت ۰..۶ = AD1..AD7، بیت ۷ = کلید کانفیگ
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
  // مقاومت‌های 4.7k پایین‌کش بیرونی هستند؛ GPIO34..39 اصلاً pull داخلی ندارند.
  for (uint8_t i = 0; i < ADDR_BITS; i++) pinMode(PIN_ADDR[i], INPUT);
  pinMode(PIN_CONFIG, INPUT);
  uint8_t votes[8] = {0};
  for (uint8_t n = 0; n < INPUT_BOOT_VOTES; n++) {
    uint8_t v = sample();
    for (uint8_t b = 0; b < 8; b++) if (v & (1 << b)) votes[b]++;
    delay(INPUT_BOOT_VOTE_GAP_MS);
  }
  uint8_t v = 0;
  for (uint8_t b = 0; b < 8; b++) if (votes[b] > INPUT_BOOT_VOTES / 2) v |= 1 << b;
  stable = candidate = v;
  atBoot = v & 0x80;
}

void loop() {
  uint32_t now = millis();
  if (now - lastSample < INPUT_SAMPLE_MS) return;
  lastSample = now;
  uint8_t v = sample();
  if (v != candidate) {                       // تغییر تازه: شمارش ثبات از اول
    candidate = v;
    candidateSince = now;
  } else if (v != stable && now - candidateSince >= INPUT_STABLE_MS) {
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

// =====================================================================================
//  ۴) چراغ‌ها (leds)
//  RUN = سلامت برد، DATA = اتصال (جزئیات الگوها در core.h).
//  همهٔ زمان‌بندی چشمک‌ها از millis() محاسبه می‌شود، پس هیچ تأخیر (delay) ندارد.
// =====================================================================================
namespace leds {
namespace {
Pattern pattern = RUN_STEADY;
bool linked = false;                        // DATA: اتصال برقرار است؟
volatile uint32_t activityUntil = 0;        // تا این لحظه جرقهٔ فعالیت روی DATA
uint32_t identifyUntil = 0;

bool runLevel(uint32_t now) {
  switch (pattern) {
    case RUN_STEADY: return true;
    case RUN_OK: return (now / LED_RUN_OK_HALF_MS) & 1;         // ۱ هرتز
    case RUN_FAULT: return (now / LED_RUN_FAULT_HALF_MS) & 1;   // ۵ هرتز
  }
  return false;
}
}  // namespace

void begin() {
  pinMode(PIN_LED_RUN, OUTPUT);
  pinMode(PIN_LED_DATA, OUTPUT);
  digitalWrite(PIN_LED_RUN, HIGH);          // در لحظهٔ شروع: RUN ثابت روشن
  digitalWrite(PIN_LED_DATA, LOW);
}

void loop() {
  uint32_t now = millis();
  if (identifyUntil && (int32_t)(identifyUntil - now) > 0) {   // حالت «شناسایی»: دو چراغ متناوب
    bool on = (now / LED_IDENTIFY_HALF_MS) & 1;
    digitalWrite(PIN_LED_RUN, on);
    digitalWrite(PIN_LED_DATA, !on);
    return;
  }
  identifyUntil = 0;
  digitalWrite(PIN_LED_RUN, runLevel(now));
  bool activity = (int32_t)(activityUntil - now) > 0;
  digitalWrite(PIN_LED_DATA, linked != activity);   // وصل: ثابت روشن و جرقه آن را کوتاه خاموش می‌کند؛ قطع: برعکس
}

void run(Pattern p) {
  if (p == pattern) return;
  pattern = p;
  LOGI("led", "RUN: %s", p == RUN_STEADY ? "steady (config mode)" : p == RUN_OK ? "slow blink (healthy)"
                                                                               : "fast blink (fault)");
}
void link(bool up) { linked = up; }
void data() { activityUntil = millis() + LED_ACTIVITY_MS; }
void identify(uint32_t ms) { identifyUntil = millis() + ms; }
}  // namespace leds

// =====================================================================================
//  ۵) کنسول سریال خط تولید (console)
//  با ۱۱۵۲۰۰ baud، هر خط یک دستور. هویت دستگاه (شناسه و رمز پلتفرم) بدون باز کردن منو نوشته می‌شود.
//  همان قرارداد hub-provision (hub/provision.py در مخزن هاب) است:
//    id <device_id>          secret <device secret>       server <host> [port] [tls]
//    api <https://hub...>    wifi <ssid> [password]       mode wifi|rs485
//    qr <متن QR برچسب>        serial <شمارهٔ سریال>         (برچسب محصول)
//    show                    save                         restart
//    led                     آزمایش چراغ‌ها (RUN و DATA شش ثانیه یکی‌درمیان)
//    factory yes | wipe yes  همه‌چیز پاک می‌شود، هویت و برچسب هم، تا برد دوباره برنامه‌ریزی شود
//                            (hub-provision بردی را که شناسه دارد نمی‌پذیرد). ریست کارخانهٔ مشتری
//                            (منو یا کلید کانفیگ) هویت و برچسب را نگه می‌دارد.
//  رمزها و کد فعال‌سازی هیچ‌وقت چاپ نمی‌شوند.
//  تغییرها اول در یک «پیش‌نویس» جمع می‌شوند و فقط با دستور save (پس از اعتبارسنجی) ذخیره می‌شوند.
// =====================================================================================
namespace console {
namespace {
constexpr uint8_t CONSOLE_LINE_CHARS = 200;   // حداکثر طول یک دستور
char line[CONSOLE_LINE_CHARS];
uint8_t len = 0;
Settings* draft = nullptr;      // پیش‌نویس تنظیمات تا دستور save

Settings& d() {
  if (!draft) draft = new Settings(settings::cur);
  return *draft;
}

// سرور پلتفرمی که دستورهای id/secret/server روی آن اعمال می‌شود (اولین فعال، وگرنه اولین خانه)
ServerCfg& platformServer() {
  int8_t i = -1;
  for (uint8_t k = 0; k < MAX_SERVERS; k++)
    if (d().servers[k].proto == PROTO_PLATFORM && d().servers[k].enabled) { i = k; break; }
  if (i < 0) i = 0;
  ServerCfg& v = d().servers[i];
  v.enabled = true;
  v.proto = PROTO_PLATFORM;
  return v;
}

void reply(const char* s) { Serial.printf("> %s\n", s); }   // پاسخ‌ها با «> » شروع می‌شوند (hub-provision همین را می‌خواند)

void show() {
  const Settings& s = draft ? *draft : settings::cur;
  Serial.printf("> model %s fw %s%s\n", FW_MODEL, FW_VERSION, draft ? "  (unsaved changes)" : "");
  Serial.printf("> mode %s  device id \"%s\"  api \"%s\"\n",
                s.mode == MODE_WIFI ? "wifi" : s.mode == MODE_RS485 ? "rs485" : "unset", s.deviceId, s.apiUrl);
  Serial.printf("> wifi \"%s\" (password %s)\n", s.nets[0].ssid, s.nets[0].pass[0] ? "set" : "empty");
  for (uint8_t i = 0; i < MAX_SERVERS; i++) {
    const ServerCfg& v = s.servers[i];
    if (!v.enabled) continue;
    Serial.printf("> server %u: %s %s:%u%s user \"%s\" secret %s\n", i + 1,
                  v.proto == PROTO_PLATFORM ? "platform" : "generic", v.host, v.port,
                  v.tls ? " tls" : "", v.user, v.pass[0] ? "set" : "empty");
  }
  settings::Label l = settings::label();
  int at = l.qr.indexOf("/claim/");
  String ident = at < 0 ? String() : l.qr.substring(at + 7, l.qr.indexOf('/', at + 7));
  Serial.printf("> label: serial \"%s\", identifier \"%s\", qr %s\n", l.serial.c_str(), ident.c_str(),
                l.qr.length() ? "stored" : "empty");
}

void run(char* cmd) {
  char* arg = strchr(cmd, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
  if (!strcmp(cmd, "help") || !*cmd) {
    reply("id, secret, server <host> [port] [tls], api <url>, wifi <ssid> [pass], mode wifi|rs485, qr <text>, serial <sn>, show, led, save, restart, factory yes, wipe yes");
  } else if (!strcmp(cmd, "show")) {
    show();
  } else if (!strcmp(cmd, "led")) {
    leds::identify(6000);   // RUN و DATA شش ثانیه یکی‌درمیان: اگر چشمک نزنند، ایراد از سخت‌افزار یا مسیر LED است
    reply("RUN and DATA alternate for 6 s (if they do not blink, check the LED wiring: RUN GPIO27, DATA GPIO14)");
  } else if (!strcmp(cmd, "id") && arg && *arg) {
    strlcpy(d().deviceId, arg, sizeof d().deviceId);
    strlcpy(platformServer().user, arg, sizeof platformServer().user);   // نام کاربری MQTT همان شناسهٔ دستگاه است
    reply("device id set (also the server username)");
  } else if (!strcmp(cmd, "secret") && arg && *arg) {
    strlcpy(platformServer().pass, arg, sizeof platformServer().pass);
    reply("secret set");
  } else if (!strcmp(cmd, "server") && arg && *arg) {
    ServerCfg& v = platformServer();
    char host[64] = "";
    unsigned port = 0;
    char tls[8] = "";
    sscanf(arg, "%63s %u %7s", host, &port, tls);
    strlcpy(v.host, host, sizeof v.host);
    v.tls = !strcmp(tls, "tls") || port == 8883;
    v.port = port ? port : (v.tls ? 8883 : DEFAULT_MQTT_PORT);
    reply("platform server set");
  } else if (!strcmp(cmd, "api") && arg) {
    strlcpy(d().apiUrl, arg, sizeof d().apiUrl);
    reply("hub address set");
  } else if (!strcmp(cmd, "wifi") && arg && *arg) {
    char* pass = strchr(arg, ' ');
    if (pass) *pass++ = 0;
    strlcpy(d().nets[0].ssid, arg, sizeof d().nets[0].ssid);
    strlcpy(d().nets[0].pass, pass ? pass : "", sizeof d().nets[0].pass);
    reply("wifi set");
  } else if (!strcmp(cmd, "mode") && arg) {
    if (!strcmp(arg, "wifi")) d().mode = MODE_WIFI;
    else if (!strcmp(arg, "rs485")) d().mode = MODE_RS485;
    else return reply("mode wifi | mode rs485");
    reply("mode set");
  } else if (!strcmp(cmd, "save")) {
    if (!draft) return reply("nothing to save");
    String err;
    Settings tmp = *draft;
    if (!settings::validate(tmp, err)) return reply((String("refused: ") + err).c_str());
    if (!settings::save(tmp)) return reply("storage error");
    delete draft;
    draft = nullptr;
    reply("saved; restart to use");
  } else if (!strcmp(cmd, "qr") && arg) {
    settings::Label l = settings::label();
    reply(settings::setLabel(arg, l.serial) ? "label QR stored" : "refused: an http(s) link up to 200 characters");
  } else if (!strcmp(cmd, "serial") && arg) {
    settings::Label l = settings::label();
    reply(settings::setLabel(l.qr, arg) ? "serial number stored" : "refused: up to 40 characters");
  } else if ((!strcmp(cmd, "wipe") || !strcmp(cmd, "factory")) && arg && !strcmp(arg, "yes")) {
    // کنسول تولید هویت را هم پاک می‌کند تا برد دوباره قابل برنامه‌ریزی باشد؛
    // ریست کارخانهٔ مشتری (منو یا کلید کانفیگ) آن را نگه می‌دارد.
    settings::factoryReset(true);
    health::restart("erased from the serial console");
  } else if (!strcmp(cmd, "restart")) {
    health::restart("serial console");
  } else {
    reply("unknown command (help)");
  }
}
}  // namespace

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = 0;
      len = 0;
      run(line);
      continue;
    }
    if (len < sizeof line - 1) line[len++] = c;
  }
}
}  // namespace console

// =====================================================================================
//  ۶) برنامهٔ اصلی: انتخاب حالت کار و حلقهٔ اصلی
// =====================================================================================
namespace {
enum RunMode : uint8_t { RUN_PORTAL, RUN_WIFI, RUN_RS485 };
RunMode runMode = RUN_PORTAL;
bool lossApplied = false;          // اقدام «قطع ارتباط» انجام شده؟
uint32_t cfgOnSince = 0;
bool pairingSent = false;
uint8_t cfgToggles = 0;            // تعداد دفعات روشن شدن کلید کانفیگ (حرکت ریست کارخانه)
uint32_t cfgToggleStart = 0;
bool cfgLast = false;

// سرتیتر شروع در سریال: نام، نسخه، MAC، علت آخرین ریست و شمارهٔ بوت
void banner() {
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.printf(" %s  %s  fw %s\n", FW_NAME, FW_MODEL, FW_VERSION);
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  Serial.printf(" MAC %02X:%02X:%02X:%02X:%02X:%02X  reset: %s  boot #%lu\n", m[0], m[1], m[2], m[3],
                m[4], m[5], health::resetReason(), (unsigned long)health::bootCount());
  Serial.println(F("=============================================="));
}

// قطع ارتباط: اگر مدت linkTimeoutS از Master مدباس (فریم معتبر) یا از سرور MQTT خبری نباشد،
// اقدام تنظیم‌شده (همه خاموش یا حالت از پیش تعیین‌شده) فقط یک بار انجام می‌شود؛
// با برگشت ارتباط، دوباره آمادهٔ اجرا می‌شود.
void linkLossCheck(uint32_t now) {
  const Settings& s = settings::cur;
  if (!s.linkTimeoutS || s.linkLoss == LOSS_HOLD) return;
  uint32_t lastOk;
  if (runMode == RUN_RS485) {
    lastOk = modbus::lastFrameMs();
  } else {
    uint32_t down = cloud::downSinceMs();
    lastOk = down ? down : now;
  }
  bool lost = now - lastOk > s.linkTimeoutS * 1000UL;   // lastOk = 0 یعنی از لحظهٔ روشن شدن هیچ پیامی نیامده
  if (lost && !lossApplied) {
    lossApplied = true;
    LOGW("main", "link lost for %u s: %s", s.linkTimeoutS,
         s.linkLoss == LOSS_ALL_OFF ? "all outputs OFF" : "preset outputs");
    relays::Cmd c = {};
    c.op = s.linkLoss == LOSS_ALL_OFF ? relays::OP_ALL_OFF : relays::OP_MASK;
    c.mask = s.linkLossMask;
    c.src = relays::SRC_LINK;
    c.force = true;               // اقدام ایمنی: حداقل فاصلهٔ رله‌ها را نادیده بگیر
    relays::post(c);
  } else if (!lost && lossApplied) {
    lossApplied = false;
    LOGI("main", "link restored");
  }
}

// «خطا» یعنی خود برد مشکل دارد: حالت ایمن، حافظهٔ کم، آدرس مدباس نامعتبر یا سرور تعریف‌نشده.
// قطع بودن سرور خطای برد نیست (ممکن است اینترنت ساختمان قطع باشد)؛ آن را چراغ DATA نشان می‌دهد.
const char* faultReason() {
  if (health::safeMode()) return "safe mode";
  if (health::lowHeap()) return "low memory";
  if (runMode == RUN_RS485) return modbus::address() == 0 ? "Modbus address 0" : nullptr;
  return cloud::configured() ? nullptr : "no server configured";
}

void runLed(uint32_t now) {
  static const char* lastReason = nullptr;
  const char* why = faultReason();
  if (why != lastReason) {
    lastReason = why;
    if (why) LOGW("led", "fault: %s", why);
  }
  leds::run(why ? leds::RUN_FAULT : leds::RUN_OK);
  if (runMode == RUN_RS485) {
    uint32_t last = modbus::lastFrameMs();
    leds::link(last && now - last < MODBUS_LINK_FRESH_MS);
  } else {
    leds::link(cloud::connected());
  }
}

// حالت Wi-Fi: کلید کانفیگ را حین کار PAIRING_HOLD_MS روشن نگه دارید = درخواست جفت‌سازی با اپ.
void pairingCheck(uint32_t now) {
  if (!inputs::configOn()) {
    cfgOnSince = 0;
    pairingSent = false;
    return;
  }
  if (!cfgOnSince) cfgOnSince = now ? now : 1;
  if (!pairingSent && now - cfgOnSince >= PAIRING_HOLD_MS) {
    pairingSent = true;
    cloud::pairing();
    leds::identify(3000);
    LOGI("main", "config switch ON: turn it OFF again; power-cycle with it ON for the portal");
  }
}

// حالت منو: کلید کانفیگ را ظرف FACTORY_GESTURE_WINDOW_MS به تعداد FACTORY_GESTURE_TOGGLES بار
// خاموش و روشن کنید = ریست کارخانه (راه برگشت وقتی رمز مدیر فراموش شده).
void factoryGesture(uint32_t now) {
  bool on = inputs::configOn();
  if (on == cfgLast) return;
  cfgLast = on;
  if (!on) return;
  if (!cfgToggles || now - cfgToggleStart > FACTORY_GESTURE_WINDOW_MS) { cfgToggles = 0; cfgToggleStart = now; }
  if (++cfgToggles >= FACTORY_GESTURE_TOGGLES) {
    LOGW("main", "config switch toggled %u times: factory reset", FACTORY_GESTURE_TOGGLES);
    leds::identify(3000);
    settings::factoryReset();
    health::restart("factory reset with the config switch");
  } else if (cfgToggles >= 3) {
    LOGW("main", "factory reset in %u more toggles of the config switch", FACTORY_GESTURE_TOGGLES - cfgToggles);
  }
}
}  // namespace

void setup() {
  // ترتیب مهم است: اول گزارش و چراغ و Watchdog، بعد تنظیمات، بعد رله‌ها (وضعیت قبلی/حالت ایمن)
  logx::begin(SERIAL_BAUD);
  leds::begin();
  health::begin();
  inputs::begin();
  settings::load();
  logx::setLevel(settings::cur.logLevel);
  banner();
  relays::onResult(cloud::ackResult);            // نتیجهٔ هر فرمان رله به سرور گزارش شود
  // حالت ایمن یا حالت تنظیمات: همه خاموش (در منو، وضعیت ذخیره‌شده هم دست نمی‌خورد)
  relays::begin(health::safeMode(), inputs::configAtBoot() || settings::cur.mode == MODE_UNSET);
  local::begin();
  rules::begin();
  health::watchThisTask();
  cfgLast = inputs::configOn();

  // انتخاب حالت کار
  if (inputs::configAtBoot() || settings::cur.mode == MODE_UNSET) {
    if (!inputs::configAtBoot()) LOGW("main", "not configured yet: starting the config portal");
    runMode = RUN_PORTAL;
    portal::begin();
  } else if (settings::cur.mode == MODE_WIFI) {
    runMode = RUN_WIFI;
    net::begin(false);
    hubapi::begin();
    cloud::begin();
  } else {
    runMode = RUN_RS485;
    modbus::begin();
  }
  LOGI("main", "running: %s", runMode == RUN_PORTAL ? "config portal"
                               : runMode == RUN_WIFI ? "Wi-Fi / MQTT" : "RS-485 / Modbus RTU");
}

void loop() {
  uint32_t now = millis();
  health::feed();            // به Watchdog بگو برنامه زنده است
  inputs::loop();
  relays::loop();
  leds::loop();
  health::loop();
  logx::flushRepeats();
  console::loop();
  switch (runMode) {
    case RUN_PORTAL:
      portal::loop();
      factoryGesture(now);
      break;
    case RUN_WIFI:
      net::loop();
      cloud::loop();
      local::loop();
      rules::loop(cloud::connected());           // قواعد آفلاین فقط وقتی سرور در دسترس نیست اجرا می‌شوند
      linkLossCheck(now);
      pairingCheck(now);
      runLed(now);
      break;
    case RUN_RS485:
      linkLossCheck(now);
      runLed(now);                               // خود Modbus در Task جدا کار می‌کند
      break;
  }
  delay(2);
}
