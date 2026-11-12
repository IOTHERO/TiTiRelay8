// =====================================================================================
//  core.h — رابط ماژول‌ها (فهرست توابعی که بخش‌های برنامه به هم می‌دهند)
// =====================================================================================
//
//  همهٔ فایل‌های برنامه فقط همین فایل را include می‌کنند. پیاده‌سازی هر بخش:
//
//    TiTiRelay8.ino : برنامهٔ اصلی + log، health، inputs، leds، console (بخش‌های کوچک)
//    settings.cpp   : ذخیره و بازیابی تنظیمات (اعلان آن در config.h است)
//    relays.cpp     : رله‌ها (صف فرمان، اینترلاک، پالس، ذخیرهٔ وضعیت)
//    net.cpp        : وای‌فای و ساعت (NTP)
//    cloud.cpp      : اتصال MQTT به سرور
//    hubapi.cpp     : HTTP(S) به هاب: کلید محلی، قواعد، آپدیت Firmware
//    local.cpp      : کنترل محلی از وای‌فای خانه
//    rules.cpp      : قواعد آفلاین (وقتی سرور در دسترس نیست)
//    modbus.cpp     : Modbus RTU Slave روی RS-485
//    portal.cpp     : منوی تنظیمات (وب)
//
//  این فایل عمداً سبک است (فقط Arduino.h)؛ کتابخانه‌های سنگین مثل ArduinoJson و
//  ESPAsyncWebServer فقط در همان فایل‌هایی include می‌شوند که لازم دارند تا کامپایل سریع بماند.
// =====================================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// -------------------------------------------------------------------------------------
//  گزارش سریال (Log)
//  سطح‌ها، یک خط برای هر رویداد، پیام‌های تکراری خلاصه می‌شوند، و چند خط آخر برای نمایش
//  زنده در منو نگه داشته می‌شود. از هر Task قابل صدا زدن است.
// -------------------------------------------------------------------------------------
enum LogLevel : uint8_t { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

namespace logx {
void begin(uint32_t baud);
void setLevel(uint8_t level);
uint8_t level();
void write(LogLevel lvl, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void flushRepeats();   // از loop(): پس از پایان بازهٔ سکوت «(پیام N بار دیگر تکرار شد)» را چاپ می‌کند

static constexpr uint8_t RING = LOG_RING_ENTRIES;
static constexpr uint8_t LINE = LOG_LINE_CHARS;
struct Entry { uint32_t seq; uint32_t ms; uint8_t lvl; char text[LINE]; };
// ورودی‌های با seq بزرگ‌تر از after را (قدیمی‌ترین اول) در out می‌ریزد؛ تعداد را برمی‌گرداند.
uint8_t since(uint32_t after, Entry* out, uint8_t max);
uint32_t lastSeq();
}

#define LOGE(tag, ...) logx::write(LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) logx::write(LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) logx::write(LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) logx::write(LOG_DEBUG, tag, __VA_ARGS__)

// -------------------------------------------------------------------------------------
//  سلامت سیستم: Watchdog، علت ریست، تشخیص ریست‌های پشت‌سرهم (حالت ایمن) و پایش حافظه
// -------------------------------------------------------------------------------------
namespace health {
void begin();                  // پیش از هر کار دیگری که ممکن است گیر کند
void watchThisTask();          // Task فعلی را زیر نظر Watchdog می‌گذارد
void feed();                   // هر Task پایش‌شده در حلقهٔ خودش صدا می‌زند
void loop();
bool lowHeap();                // حافظهٔ آزاد کمتر از حد مجاز
bool safeMode();               // ۳ ریست غیرعادی پشت سر هم: رله‌ها خاموش بالا می‌آیند
const char* resetReason();
uint32_t bootCount();
void restart(const char* why); // وضعیت را ذخیره می‌کند و ریست می‌کند
}

// -------------------------------------------------------------------------------------
//  ورودی‌ها: کلید کانفیگ با حذف لرزش (دیپ‌سوئیچ آدرس حذف شد)
// -------------------------------------------------------------------------------------
namespace inputs {
void begin();
void loop();                 // هر ۱۰ms نمونه می‌گیرد؛ تغییر باید ۵۰ms ثابت بماند
bool configAtBoot();         // هنگام روشن شدن خوانده می‌شود: کلید کانفیگ حالت کار را تعیین می‌کند
bool configOn();             // وضعیت زندهٔ کلید کانفیگ
uint8_t raw();               // بیت ۷ = کلید کانفیگ (Modbus همان بیت را گزارش می‌کند)
}

// -------------------------------------------------------------------------------------
//  چراغ‌ها: RUN = سلامت برد، DATA = اتصال
//    RUN ثابت        : حالت تنظیمات / لحظهٔ شروع
//    RUN چشمک آرام   : سالم (۱ هرتز)
//    RUN چشمک تند    : خطا (حالت ایمن، آدرس مدباس نامعتبر، سرور تعریف نشده، حافظهٔ کم)
//    DATA ثابت روشن  : به سرور وصل است (Wi-Fi) / Master پاسخ می‌گیرد (RS-485)
//    DATA خاموش      : اتصال نیست
//    DATA جرقه       : هر پیام یا فریم (هنگام اتصال خاموش‌شدن کوتاه، هنگام قطع روشن‌شدن کوتاه)
// -------------------------------------------------------------------------------------
namespace leds {
enum Pattern : uint8_t { RUN_STEADY, RUN_OK, RUN_FAULT };
void begin();
void loop();
void run(Pattern p);
void link(bool up);
void data();                     // یک جرقهٔ فعالیت (از هر Task)
void identify(uint32_t ms);      // هر دو چراغ تند و متناوب چشمک می‌زنند (پیدا کردن برد)
}

// -------------------------------------------------------------------------------------
//  رله‌ها: هر تغییر، از هر منبع و هر Task، از post() می‌گذرد. حلقهٔ اصلی آن را با رعایت
//  حداقل فاصله، فاصلهٔ وصل کویل‌ها و اینترلاک اجرا می‌کند و وضعیت را در NVS نگه می‌دارد.
// -------------------------------------------------------------------------------------
namespace relays {
// منبع فرمان (برای گزارش در Log و ثبت در local_log)
enum Source : uint8_t { SRC_BOOT, SRC_WEB, SRC_MQTT, SRC_MODBUS, SRC_LINK, SRC_SYSTEM, SRC_LOCAL, SRC_RULE };
enum Op : uint8_t { OP_SET, OP_MASK, OP_ALL_OFF };

struct Cmd {
  uint8_t op;
  uint8_t relay;          // شمارهٔ رله از صفر (برای OP_SET)
  bool value;             // OP_SET
  uint8_t mask;           // OP_MASK: بیت i = رلهٔ i روشن
  uint8_t src;            // Source
  bool force;             // اقدام ایمنی: حداقل فاصله را نادیده می‌گیرد
  uint32_t pulseMs;       // OP_SET با value=true: بعد از این مدت دوباره خاموش شود (پالس)
  char id[40];            // شناسهٔ فرمان برای گزارش نتیجه به سرور (خالی = ندارد)
};

// نتیجهٔ فرمانی که شناسه داشت (از حلقهٔ اصلی صدا زده می‌شود)
using ResultFn = void (*)(const char* id, bool ok, uint8_t relay, bool value, const char* err);

void begin(bool safeMode, bool configMode = false);   // configMode: همه خاموش و وضعیت ذخیره نمی‌شود
void loop();
bool post(const Cmd& c);                      // از هر Task؛ false اگر صف پر است
bool set(uint8_t relay, bool on, Source src, uint32_t pulseMs = 0);
bool setMask(uint8_t mask, Source src);
bool allOff(Source src, bool force);
void onResult(ResultFn fn);

uint8_t state();                 // بیت i = رلهٔ i اکنون وصل است
uint8_t pulsing();
bool fitted(uint8_t relay);
uint8_t fittedMask();
uint32_t cycles(uint8_t relay);  // دفعات وصل شدن رله در طول عمر
uint32_t changes();              // با هر تغییر زیاد می‌شود (محرک ارسال وضعیت)
const char* sourceName(uint8_t src);
void saveNow();                  // پیش از ریست برنامه‌ریزی‌شده
}

// -------------------------------------------------------------------------------------
//  وای‌فای: شبکهٔ اصلی و پشتیبان، IP ثابت یا DHCP، اتصال دوباره با تأخیر افزایشی، ساعت NTP
// -------------------------------------------------------------------------------------
namespace net {
void begin(bool keepAp);     // keepAp: منوی تنظیمات هم‌زمان بالا می‌ماند (AP + STA)
void loop();
void stop();
bool up();                   // شبکه (وای‌فای یا LAN) آدرس دارد
bool lan();                  // اتصال فعلی از راه کابل LAN (W5500) است
bool lanLink();              // کابل LAN وصل است (لینک)
int rssi();                  // فقط وای‌فای؛ روی LAN صفر
String ip();
String ssid();               // روی LAN: "LAN"
String mac();
const char* status();        // متن کوتاه برای منو
bool timeValid();            // ساعت درست است؟ (TLS و قواعد زمانی به آن نیاز دارند)
uint64_t epochMs();          // زمان UTC؛ 0 اگر ساعت معلوم نیست
}

// -------------------------------------------------------------------------------------
//  اتصال MQTT به سرور (به ترتیب امتحان می‌شوند؛ بعد از شکست سرور بعدی)
//
//  قرارداد پلتفرم (backend/mqtt_ingest.py در هاب):
//    <prefix>d/<id>/telemetry  {"ts": <epoch ms>, "values": {"relay_1": true, ..., "uptime_s": 1}}
//    <prefix>d/<id>/state      {"state": "online"|"offline", "firmware", "local_addr"}  (retained + LWT)
//    <prefix>d/<id>/ack        {"command_id", "status": "received"|"done"|"failed", "value", "error"}
//    <prefix>d/<id>/pairing    {}
//    <prefix>d/<id>/config     {"type": "config", "local": {...}}   (هاب ← برد: کلید کنترل محلی)
//    <prefix>d/<id>/rules      {"type": "rules", ...}               (هاب ← برد: قواعد آفلاین)
//    <prefix>d/<id>/ota        پیشنهاد Firmware؛ گزارش روی d/<id>/ota/status
//    <prefix>d/<id>/local_log  آنچه بدون سرور انجام شد
//    <prefix>d/<id>/cmd        {"command_id", "tag": "relay_1", "value": true, "expires_at", "pulse_s"}
//    نام کاربری = device id، رمز = device secret.
//  MQTT عمومی (مثل درایور mqtt_json هاب): <base>/telemetry، <base>/status، <base>/cmd، <base>/ack
// -------------------------------------------------------------------------------------
namespace cloud {
void begin();
bool configured();             // حداقل یک سرور فعال تعریف شده
void loop();
void stop();
bool connected();
int serverIndex();             // -1 اگر وصل نیست
const char* lastError();
uint32_t downSinceMs();        // لحظه‌ای که ارتباط قطع شد؛ 0 تا وقتی وصل است
void pairing();                // درخواست جفت‌سازی با اپ (سرورهای پلتفرم)
uint32_t rxCount();
uint32_t txCount();
// هدف relays::onResult: ack «done» یا «failed» برای فرمان‌های پلتفرم
void ackResult(const char* id, bool ok, uint8_t relay, bool value, const char* error);
// ارسال پیام d/<id>/<kind> (مثل ota/status یا local_log)؛ false اگر وصل نیست
bool publishKind(const char* kind, const String& json);

// منوی تنظیمات: یک تلاش اتصال به سرور idx؛ نتیجه در testResult()
void test(uint8_t idx);
String testResult();
}

// -------------------------------------------------------------------------------------
//  HTTP(S) به هاب با اعتبارنامهٔ دستگاه (Settings::apiUrl، Bearer <secret>):
//    GET /ingest/v1/devices/<id>/config          کلید محلی (پس از هر اتصال)
//    GET /ingest/v1/devices/<id>/rules           قواعد آفلاین (پس از هر اتصال)
//    GET /ingest/v1/devices/<id>/firmware/<rel>  تصویر Firmware یک پیشنهاد
//  در Task جدا اجرا می‌شود تا کندی سرور رله‌ها را معطل نکند.
// -------------------------------------------------------------------------------------
namespace hubapi {
void begin();
bool configured();                          // apiUrl و اعتبارنامهٔ پلتفرم تعریف شده
void fetchAll();                            // config + rules (پس از اتصال به پلتفرم)
void offerOta(const char* json, size_t len);// پیام d/<id>/ota (از هر Task)
void reportPendingOta();                    // پس از اتصال: نتیجهٔ آپدیتِ پیش از ریست
bool otaBusy();
}

// -------------------------------------------------------------------------------------
//  کنترل محلی از وای‌فای خانه (قرارداد هاب، docs/platform.md «Local control»):
//  وقتی اینترنت قطع است، اپ فرمان امضاشده را مستقیم به برد می‌فرستد.
//    GET  /local/v1/info     {"device_id", "firmware", "local"}
//    GET  /local/v1/nonce    {"device_id", "nonce", "ttl_s": 30}   (یک‌بارمصرف)
//    POST /local/v1/command  {"nonce", "tag", "value_json", "sig"}
//    POST /local/v1/state    {"nonce", "sig"}
//  امضا = base64url(HMAC-SHA256(key, "<device_id>\n<nonce>\ncommand\n<tag>\n<value_json>"))
//  کلید از هاب می‌آید و در NVS می‌ماند. برد با mDNS به نام _titiswitch._tcp (TXT: id=<device_id>)
//  خودش را معرفی می‌کند و کارهای محلی را بعداً در local_log گزارش می‌دهد.
// -------------------------------------------------------------------------------------
namespace local {
void begin();
void loop();
// پیام کامل config از هاب (JSON: {"type":"config","local":{enabled,key,version,port}}) — از هر Task
void applyConfig(const char* json, size_t len);
bool keySet();
uint16_t port();
String addr();                              // نشانی خصوصی برد در شبکهٔ خانه؛ خالی اگر ندارد
// تغییری که بدون سرور انجام شد (فرمان محلی یا قاعدهٔ آفلاین)؛ بعداً گزارش می‌شود
void log(uint8_t relay, bool value, const char* by);
void flushLog();                            // ارسال موارد منتظر (وقتی به پلتفرم وصل است)
}

// -------------------------------------------------------------------------------------
//  قواعدی که برد خودش اجرا می‌کند وقتی از سرور قطع است (قرارداد هاب، backend/api/home.py):
//  {"type":"rules","version","rules":[...],"utc_offset_min","holidays":["YYYY-MM-DD",...],
//   "offline_after_s","server_time"}. در NVS می‌ماند؛ فقط وقتی سرور به مدت offline_after_s در
//  دسترس نبوده اجرا می‌شود تا هیچ قاعده‌ای دوبار اجرا نشود. ماشه: time، interval، value؛
//  شرط روی مقدارهای خود برد؛ کار: write و delay. روزها به تقویم جلالی (۰ = شنبه) مثل سرور.
// -------------------------------------------------------------------------------------
namespace rules {
void begin();
void loop(bool serverReachable);
void apply(const char* json, size_t len);   // پیام قواعد (از هر Task)
bool setClock(const char* iso, bool force);  // ساعت از سرور/هاب (ISO UTC)؛ force: هاب، حتی اگر ساعت معتبر است
void setOffset(int32_t minutes);             // اختلاف ساعت محلی (ایران ۲۱۰)
uint8_t count();
const char* version();
}

// -------------------------------------------------------------------------------------
//  Modbus RTU Slave روی RS-485 (Task جداگانه). نقشهٔ رجیسترها: README.md و منوی تنظیمات.
// -------------------------------------------------------------------------------------
namespace modbus {
void begin();
void stop();
uint8_t address();
uint32_t lastFrameMs();      // millis() آخرین درخواست معتبر برای این Slave؛ 0 = هیچ‌وقت
uint32_t goodFrames();
uint32_t crcErrors();
}

// -------------------------------------------------------------------------------------
//  منوی تنظیمات: وای‌فای 192.168.1.100 وقتی کلید کانفیگ (دیپ ۸) هنگام روشن شدن روشن بود.
//  ورود با رمز، وضعیت زنده با WebSocket، تنظیمات، تست رله و ورودی، آپدیت Firmware.
// -------------------------------------------------------------------------------------
namespace portal {
void begin();
void loop();
}

// -------------------------------------------------------------------------------------
//  کنسول سریال خط تولید (۱۱۵۲۰۰ baud، هر خط یک دستور) — پیاده‌سازی در TiTiRelay8.ino
// -------------------------------------------------------------------------------------
namespace console {
void loop();
}
