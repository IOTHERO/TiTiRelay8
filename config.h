// =====================================================================================
//  config.h — تنظیمات مرکزی TiTi Relay-8
// =====================================================================================
//
//  همهٔ چیزهایی که قابل تنظیم هستند در همین فایل جمع شده‌اند:
//    ۱) هویت و نسخهٔ Firmware
//    ۲) پایه‌های سخت‌افزار (رله‌ها، LED، RS-485، کلید کانفیگ، LAN)
//    ۳) زمان‌بندی‌ها و محدودیت‌های هر بخش (Watchdog، Wi-Fi، MQTT، Modbus، ...)
//    ۴) مقدارهای پیش‌فرض تنظیمات (پس از ریست کارخانه همین‌ها برقرار می‌شوند)
//    ۵) ساختار تنظیماتی که کاربر از منوی وب عوض می‌کند (struct Settings)
//
//  قاعده‌ها:
//   - با عوض کردن این فایل، همهٔ فایل‌های برنامه دوباره کامپایل می‌شوند.
//   - مقدارهای «DEFAULT_...» فقط پیش‌فرض هستند؛ کاربر می‌تواند از منو عوضشان کند
//     و در حافظهٔ دستگاه (NVS) ذخیره می‌شوند.
//   - مقدارهای دیگر (زمان‌بندی، پایه‌ها) داخل برنامه ثابت هستند و فقط با کامپایل دوباره عوض می‌شوند.
//   - ساختار Settings را فقط از انتها گسترش بدهید (توضیح در بخش ۵)، وگرنه با آپدیت
//     Firmware تنظیمات دستگاه‌های نصب‌شده از بین می‌رود.
// =====================================================================================
#pragma once
#include <Arduino.h>

// -------------------------------------------------------------------------------------
//  ۱) هویت و نسخه
// -------------------------------------------------------------------------------------
#define FW_NAME     "TiTi Relay-8"      // نام نمایشی محصول
#define FW_MODEL    "TR8-ESP32"         // مدل (در منو و کنسول سریال نمایش داده می‌شود)
#define FW_VERSION  "1.0.0"             // نسخه؛ با آپدیت از پلتفرم مقایسه می‌شود
#define FW_VER_HEX  0x0100              // همان نسخه برای Modbus (رجیستر Input 3): بالا = major، پایین = minor

// -------------------------------------------------------------------------------------
//  ۲) پایه‌های سخت‌افزار (طبق شماتیک برد؛ نام نت‌ها MCU_DIx / MCU_ADx / ADM_xxx)
// -------------------------------------------------------------------------------------

// --- رله‌ها (از طریق ULN2803) ---------------------------------------------------------
// ULN2803 معکوس‌کننده است ولی ورودی آن مستقیم از GPIO می‌آید: HIGH = کویل وصل = رله روشن.
// رله ۸ روی GPIO12 است. این پایه هنگام بوت باید LOW باشد (Strapping pin با نام MTDI؛
// اگر HIGH باشد ESP32 بالا نمی‌آید). مقاومت 10k پایین‌کش روی ورودی ULN آن را LOW نگه
// می‌دارد و برنامه هم پیش از هر کار دیگر آن را LOW می‌کند.
// برای کنار گذاشتن رله ۸ مقدار زیر را 0 کنید (در منو «رزرو» نمایش داده می‌شود).
#ifndef ENABLE_RELAY8_GPIO12
#define ENABLE_RELAY8_GPIO12 1
#endif

static constexpr uint8_t RELAY_COUNT = 8;                    // تعداد رله‌ها (برد ۸ کانال)
// شمارهٔ GPIO هر رله از K1 تا K8. مقدار -1 یعنی نصب نشده.
static constexpr int8_t RELAY_PINS[RELAY_COUNT] = {
  5, 18, 19, 21, 22, 23, 13, ENABLE_RELAY8_GPIO12 ? 12 : -1};

// --- چراغ‌ها (فعال با HIGH؛ مقاومت 2.2k سری) -------------------------------------------
static constexpr uint8_t PIN_LED_DATA = 14;                  // اتصال: ثابت روشن = به سرور وصل است
static constexpr uint8_t PIN_LED_RUN  = 27;                  // سلامت: چشمک آرام = سالم، تند = خطا، ثابت = حالت تنظیمات

// --- RS-485 ----------------------------------------------------------------------------
// پایه‌های DE و /RE ترانسیور به هم وصل‌اند (نت ADM_SE): HIGH = ارسال، LOW = دریافت.
static constexpr uint8_t PIN_RS485_SE = 4;
// UART2 در ESP32: GPIO16 = U2RXD (دریافت از خروجی RO ترانسیور)،
//                 GPIO17 = U2TXD (ارسال به ورودی DI ترانسیور).
// اگر برد شما برعکس سیم‌کشی شده، از منو گزینهٔ «جابه‌جایی RX/TX» را بزنید.
static constexpr uint8_t PIN_RS485_RX = 16;
static constexpr uint8_t PIN_RS485_TX = 17;

// --- کلید کانفیگ ------------------------------------------------------------------------
// دیپ‌سوئیچ آدرس (AD1..AD7) حذف شد: آدرس Modbus از منو است و پایه‌های آزادشده به ماژول LAN
// (W5500) رسیدند. فقط کلید حالت تنظیمات می‌ماند: COMMON به +3.3V، مقاومت 4.7k پایین‌کش، روشن = HIGH.
static constexpr uint8_t PIN_CONFIG = 26;                    // کلید کانفیگ (همان AD8 قبلی)

// --- LAN: ماژول W5500 روی SPI (پایه‌های دیپ‌سوئیچ قبلی) ----------------------------------
// SCK، MOSI و CS خروجی‌اند؛ MISO و INT ورودی (GPIO34..39 فقط ورودی‌اند و برای همین مناسب‌اند).
// RST وصل نیست (W5500 با برق خودش ریست می‌شود)؛ اگر روی برد سیم شد، شمارهٔ پایه را بدهید.
// GPIO36 و GPIO39 آزاد می‌مانند (فقط ورودی). برد بدون W5500: HAS_LAN را 0 کنید.
#ifndef HAS_LAN
#define HAS_LAN 1
#endif
static constexpr int8_t PIN_ETH_SCK  = 25;
static constexpr int8_t PIN_ETH_MOSI = 33;
static constexpr int8_t PIN_ETH_CS   = 32;
static constexpr int8_t PIN_ETH_MISO = 35;
static constexpr int8_t PIN_ETH_INT  = 34;
static constexpr int8_t PIN_ETH_RST  = -1;
static constexpr uint32_t LAN_LINK_WAIT_MS = 8000;           // حالت خودکار: این‌قدر منتظر کابل، بعد وای‌فای

// -------------------------------------------------------------------------------------
//  ۳) سریال، گزارش (Log) و چراغ‌ها
// -------------------------------------------------------------------------------------
static constexpr uint32_t SERIAL_BAUD = 115200;              // سرعت پورت سریال USB (کنسول تولید و گزارش)

static constexpr uint8_t  LOG_RING_ENTRIES = 48;             // چند خط آخر گزارش در حافظه می‌ماند (برای نمایش زنده در منو)
static constexpr uint8_t  LOG_LINE_CHARS = 112;              // حداکثر طول هر خط گزارش
static constexpr uint32_t LOG_REPEAT_WINDOW_MS = 5000;       // پیام تکراری در این مدت یک‌خطی خلاصه می‌شود (سریال شلوغ نشود)
static constexpr uint8_t  DEFAULT_LOG_LEVEL = 2;             // 0 خطا، 1 هشدار، 2 اطلاعات، 3 اشکال‌زدایی

static constexpr uint16_t LED_RUN_OK_HALF_MS = 500;          // چشمک آرام (سالم): نیم‌دورهٔ ۵۰۰ms = ۱ هرتز
static constexpr uint16_t LED_RUN_FAULT_HALF_MS = 100;       // چشمک تند (خطا): نیم‌دورهٔ ۱۰۰ms = ۵ هرتز
static constexpr uint16_t LED_IDENTIFY_HALF_MS = 80;         // چشمک دوتایی هنگام «شناسایی»
static constexpr uint16_t LED_ACTIVITY_MS = 60;              // مدت یک جرقهٔ DATA هنگام پیام یا فریم

// -------------------------------------------------------------------------------------
//  ۴) ورودی‌ها (کلید کانفیگ) و سلامت سیستم
// -------------------------------------------------------------------------------------
static constexpr uint16_t INPUT_SAMPLE_MS = 10;              // هر چند ms کلید کانفیگ خوانده شود
static constexpr uint16_t INPUT_STABLE_MS = 50;              // تغییر باید این‌قدر ثابت بماند تا قبول شود (حذف لرزش کلید)
static constexpr uint8_t  INPUT_BOOT_VOTES = 9;              // هنگام روشن شدن چند بار خوانده شود (رأی اکثریت)
static constexpr uint8_t  INPUT_BOOT_VOTE_GAP_MS = 5;        // فاصلهٔ بین دو خواندن هنگام روشن شدن

static constexpr uint32_t HEALTH_WDT_TIMEOUT_MS = 8000;      // Watchdog: اگر حلقهٔ اصلی این‌قدر گیر کند، برد ریست می‌شود
static constexpr uint32_t HEALTH_STABLE_AFTER_MS = 60000;    // بعد از این‌قدر کار بدون خطا، بوت «سالم» حساب می‌شود
static constexpr uint8_t  HEALTH_CRASH_LIMIT = 3;            // سه ریست غیرعادی پشت سر هم = حالت ایمن (رله‌ها خاموش بالا می‌آیند)
static constexpr uint32_t HEALTH_LOW_HEAP_BYTES = 16 * 1024; // کمتر از این حافظهٔ آزاد = هشدار و چراغ خطا
static constexpr uint32_t HEALTH_LOW_HEAP_FOR_MS = 30000;    // اگر این‌قدر کم بماند، ریست کنترل‌شده
static constexpr uint32_t HEALTH_HEAP_REPORT_MS = 10UL * 60 * 1000;  // هر چند وقت وضعیت حافظه در گزارش (سطح اشکال‌زدایی)

// کلید کانفیگ در حین کار:
static constexpr uint8_t  FACTORY_GESTURE_TOGGLES = 5;       // در حالت منو: این‌قدر خاموش/روشن = ریست کارخانه (فراموشی رمز مدیر)
static constexpr uint32_t FACTORY_GESTURE_WINDOW_MS = 15000; // ... در این مدت
static constexpr uint32_t PAIRING_HOLD_MS = 3000;            // در حالت Wi-Fi: کلید ۸ را این‌قدر روشن نگه دارید = درخواست جفت‌سازی با اپ
static constexpr uint32_t MODBUS_LINK_FRESH_MS = 5000;       // اگر در این مدت فریمی از Master آمده باشد، چراغ DATA روشن است

// -------------------------------------------------------------------------------------
//  ۵) رله‌ها
// -------------------------------------------------------------------------------------
static constexpr uint16_t DEFAULT_MIN_INTERVAL_MS = 100;     // حداقل فاصلهٔ دو تغییر پشت سر هم یک رله (حفاظت کنتاکت از لرزش فرمان)
static constexpr uint16_t DEFAULT_STAGGER_MS = 30;           // فاصلهٔ وصل شدن دو کویل (کم‌کردن جریان هجومی)
static constexpr uint8_t  RELAY_QUEUE_LEN = 24;              // صف فرمان رله‌ها
static constexpr uint8_t  RELAY_CMDS_PER_LOOP = 16;          // در هر دور حلقه حداکثر چند فرمان اجرا شود

// ذخیرهٔ وضعیت رله‌ها در حافظه (برای بازگشت پس از قطع برق) با محافظت در برابر فرسایش فلش:
// وضعیت فقط وقتی ذخیره می‌شود که RELAY_STATE_STABLE_MS ثابت مانده باشد و حداکثر هر
// RELAY_STATE_MIN_GAP_MS یک بار. (تایمر «۱ ثانیه روشن و ۱ ثانیه خاموش» وگرنه روزی ده‌ها
// هزار بار می‌نوشت.) هزینه: تغییرِ چند ثانیهٔ آخر پیش از قطع برق بازگردانده نمی‌شود.
static constexpr uint32_t RELAY_STATE_STABLE_MS = 3000;
static constexpr uint32_t RELAY_STATE_MIN_GAP_MS = 10000;
static constexpr uint32_t RELAY_CYCLES_SAVE_MS = 10UL * 60 * 1000;  // شمارندهٔ دفعات وصل شدن هر رله هر چند وقت ذخیره شود

// -------------------------------------------------------------------------------------
//  ۶) حالت تنظیمات (Portal): شبکهٔ وای‌فای خود برد و منوی وب
// -------------------------------------------------------------------------------------
static constexpr uint8_t PORTAL_IP[4] = {192, 168, 1, 100};  // نشانی منو وقتی به وای‌فای خود برد وصل هستید
#define PORTAL_AP_SSID_PREFIX "TiTi-R8-"                     // نام شبکه = پیشوند + چهار رقم آخر MAC
static constexpr uint8_t  PORTAL_AP_CHANNEL = 6;             // کانال وای‌فای برد
static constexpr uint8_t  PORTAL_AP_MAX_CLIENTS = 4;         // حداکثر کاربر هم‌زمان
#define DEFAULT_AP_PASSWORD "titi1234"                       // رمز وای‌فای برد (۸ تا ۶۳ حرف) — در نصب نهایی عوضش کنید
#define DEFAULT_ADMIN_PASSWORD "admin"                       // رمز ورود به منو — در نصب نهایی عوضش کنید

static constexpr uint8_t  PORTAL_MAX_TOKENS = 4;             // حداکثر نشست هم‌زمان ورود
static constexpr uint32_t PORTAL_TOKEN_TTL_MS = 8UL * 3600 * 1000;  // اعتبار هر نشست ورود (۸ ساعت)
static constexpr uint8_t  PORTAL_MAX_FAILS = 5;              // این‌قدر رمز اشتباه پشت سر هم = قفل شدن ورود
static constexpr uint32_t PORTAL_LOCKOUT_MS = 60000;         // مدت قفل (یک دقیقه)
static constexpr uint32_t PORTAL_PUSH_MS = 400;              // هر چند ms وضعیت زنده به مرورگر فرستاده شود
static constexpr uint8_t  PORTAL_MAX_WS = 4;                 // حداکثر مرورگر هم‌زمان (WebSocket)
static constexpr uint32_t PORTAL_WALK_STEP_MS = 1000;        // «تست ترتیبی»: هر رله چند ms روشن بماند

// -------------------------------------------------------------------------------------
//  ۷) وای‌فای (حالت Wi-Fi / MQTT)
// -------------------------------------------------------------------------------------
#define DEFAULT_NTP_SERVER "pool.ntp.org"                    // سرور زمان (اگر در محل شما مسدود است، در منو عوض کنید)
#define NTP_FALLBACK_1 "pool.ntp.org"                        // دو سرور زمان پشتیبان
#define NTP_FALLBACK_2 "time.google.com"
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;   // هر تلاش اتصال تا چند ms صبر کند
static constexpr uint32_t WIFI_BACKOFF_MIN_MS = 5000;        // فاصلهٔ تلاش مجدد: از ۵ ثانیه شروع و دو برابر می‌شود
static constexpr uint32_t WIFI_BACKOFF_MAX_MS = 60000;       // ... تا سقف یک دقیقه
static constexpr uint16_t WIFI_BACKOFF_JITTER_MS = 2000;     // چند ثانیه تصادفی اضافه (همه بردها هم‌زمان وصل نشوند)

// -------------------------------------------------------------------------------------
//  ۸) اتصال به سرور (MQTT)
// -------------------------------------------------------------------------------------
static constexpr uint16_t DEFAULT_TELEMETRY_S = 30;          // هر چند ثانیه وضعیت رله‌ها حتی بدون تغییر ارسال شود
static constexpr uint16_t DEFAULT_MQTT_PORT = 1883;          // بدون TLS (با TLS معمولاً 8883)
static constexpr uint16_t DEFAULT_MQTT_KEEPALIVE_S = 30;
static constexpr uint8_t  DEFAULT_MQTT_QOS = 1;

// تلاش مجدد اتصال طبق قرارداد سرور: اولین تلاش بعد از ۰ تا ۱۰ ثانیه تصادفی، بعد دو برابر تا ۵ دقیقه، ±۲۰٪.
// (برای اینکه با برگشت برق، هزاران دستگاه هم‌زمان به سرور نچسبند.)
static constexpr uint32_t MQTT_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t MQTT_RETRY_FIRST_MAX_MS = 10000;
static constexpr uint32_t MQTT_RETRY_CAP_MS = 300000;
static constexpr uint32_t MQTT_STABLE_MS = 60000;            // اتصالی که این‌قدر دوام بیاورد، شمارندهٔ تلاش را صفر می‌کند
static constexpr uint32_t MQTT_TELEMETRY_MIN_GAP_MS = 250;   // سرور بیش از ۵ پیام در ثانیه را دور می‌ریزد؛ ما حداکثر ۴
static constexpr uint8_t  MQTT_SEEN_IDS = 24;                // شناسهٔ چند فرمان اخیر نگه داشته می‌شود تا فرمان تکراری دوباره اجرا نشود
static constexpr uint32_t MQTT_TLS_CLOCK_WAIT_MS = 20000;    // TLS تاریخ گواهی را می‌سنجد: تا این‌قدر برای ساعت (NTP) صبر کن
static constexpr uint32_t MQTT_NETWORK_TIMEOUT_MS = 10000;
static constexpr int      MQTT_RX_BUFFER = 12288;            // پیام قواعد ممکن است چند کیلوبایت باشد
static constexpr int      MQTT_TX_BUFFER = 2048;
static constexpr int      MQTT_TASK_STACK = 6144;

// نشانی و محدودیت‌های ارتباط HTTP(S) با هاب (دریافت کلید محلی، قواعد، Firmware):
static constexpr size_t   HUBAPI_MAX_JSON_BYTES = 16 * 1024;
static constexpr int      HUBAPI_TIMEOUT_MS = 15000;
static constexpr int      HUBAPI_TASK_STACK = 8192;

// -------------------------------------------------------------------------------------
//  ۹) کنترل محلی (وقتی اینترنت قطع است ولی وای‌فای خانه هست) و قواعد آفلاین
// -------------------------------------------------------------------------------------
static constexpr uint16_t LOCAL_DEFAULT_PORT = 8080;         // پورت سرور کوچک داخل برد؛ سرور می‌تواند عوضش کند
static constexpr uint32_t LOCAL_NONCE_TTL_MS = 30000;        // هر کد یک‌بارمصرف (nonce) ۳۰ ثانیه اعتبار دارد
static constexpr uint8_t  LOCAL_MAX_NONCES = 16;             // حداکثر nonce باز هم‌زمان
static constexpr uint8_t  LOCAL_LOG_ENTRIES = 40;            // چند رویداد محلی تا اتصال دوباره نگه داشته شود
static constexpr size_t   LOCAL_MAX_BODY = 1024;             // حداکثر اندازهٔ درخواست

static constexpr size_t   RULES_MAX_JSON_BYTES = 12 * 1024;  // حداکثر اندازهٔ پیام قواعد
static constexpr uint8_t  RULES_MAX_RUNS = 8;                // حداکثر قاعدهٔ در حال اجرای هم‌زمان
static constexpr uint8_t  RULES_MAX_COUNT = 32;              // حداکثر تعداد قاعده

// -------------------------------------------------------------------------------------
//  ۱۰) Modbus RTU (برد در نقش Slave)
// -------------------------------------------------------------------------------------
static constexpr uint32_t DEFAULT_MB_BAUD = 9600;            // سرعت‌های مجاز: ۱۲۰۰ تا ۱۱۵۲۰۰ (فهرست در settings.cpp)
static constexpr uint8_t  DEFAULT_MB_PARITY = 0;             // 0 بدون، 1 زوج (Even)، 2 فرد (Odd)
static constexpr uint8_t  DEFAULT_MB_STOP_BITS = 1;
static constexpr uint8_t  DEFAULT_MB_ADDR = 1;               // آدرس دستی Slave (۱ تا ۲۴۷) وقتی آدرس از دیپ‌سوئیچ نیست
static constexpr uint16_t MB_FRAME_MAX = 256;                // بزرگ‌ترین فریم Modbus RTU (استاندارد ۲۵۶ بایت)
static constexpr int      MB_UART_RX_BUFFER = 512;           // بافر دریافت UART
static constexpr int      MB_TASK_STACK = 4096;
static constexpr uint8_t  MB_TASK_PRIORITY = 3;

// --- نقشهٔ رجیسترها (اندازهٔ هر جدول) -----------------------------------------------------
// جدول کامل با آدرس در README.md و در منوی تنظیمات (صفحهٔ ارتباط، بخش RS-485) هست.
//   Coil     (FC1/5/15) : ۰..۷       رله‌های ۱ تا ۸
//   Discrete (FC2)      : ۰..۷       ۰..۶ دیپ AD1..AD7، ۷ کلید کانفیگ
//   Input    (FC4)      : ۰..۲۵      وضعیت، آدرس، نسخه، زمان کارکرد، آمار، شمارندهٔ رله‌ها
//   Holding  (FC3/6/16) : ۰..۲۷      ۰ ماسک رله‌ها، ۱ فرمان، ۲..۹ پالس، ۲۰..۲۷ هر رله یک رجیستر
static constexpr uint16_t MB_COIL_COUNT = RELAY_COUNT;
static constexpr uint16_t MB_DISCRETE_COUNT = 8;
static constexpr uint16_t MB_INPUT_COUNT = 26;
static constexpr uint16_t MB_HOLDING_COUNT = 28;
static constexpr uint16_t MB_RELAY_REGS_START = 20;          // Holding 20..27: رجیستر جدا برای هر رله (۰/۱)

// --- آدرس شروع هر جدول روی سیم (برای سازگاری با PLCهای مختلف) -------------------------------
// بعضی PLC یا HMI آدرس را از ۱ می‌شمارند، بعضی فقط در یک بازهٔ مشخص می‌خوانند. برای هر جدول
// می‌شود «آدرس شروع» داد: آدرس روی سیم = آدرس شروع + شمارهٔ داخلی.
// مثال: Holding با شروع 100 ← ماسک رله‌ها روی آدرس 100 است نه 0.
// اندیس جدول‌ها در آرایهٔ Settings::mbBase:
enum MbTable : uint8_t { MB_T_COIL = 0, MB_T_DISCRETE = 1, MB_T_INPUT = 2, MB_T_HOLDING = 3 };
static constexpr uint16_t MB_TABLE_LEN[4] = {MB_COIL_COUNT, MB_DISCRETE_COUNT, MB_INPUT_COUNT, MB_HOLDING_COUNT};

// -------------------------------------------------------------------------------------
//  ۱۱) قطع ارتباط (Link loss)
// -------------------------------------------------------------------------------------
// اگر مدت linkTimeoutS از سرور یا Master پیامی نرسد، یک بار این اقدام انجام می‌شود.
static constexpr uint16_t DEFAULT_LINK_TIMEOUT_S = 0;        // 0 = غیرفعال

// -------------------------------------------------------------------------------------
//  ۱۲) ساختار تنظیمات (آنچه از منو تنظیم و در حافظه ذخیره می‌شود)
// -------------------------------------------------------------------------------------
enum LinkMode : uint8_t { MODE_UNSET = 0, MODE_WIFI = 1, MODE_RS485 = 2 };             // نوع ارتباط (WIFI = شبکه: وای‌فای یا LAN)
enum NetMode : uint8_t { NET_AUTO = 0, NET_WIFI = 1, NET_LAN = 2 };                     // کدام شبکه در حالت MQTT
enum PowerOn : uint8_t { PON_RESTORE = 0, PON_OFF = 1, PON_ON = 2 };                    // حالت رله پس از وصل برق
enum LinkLoss : uint8_t { LOSS_HOLD = 0, LOSS_ALL_OFF = 1, LOSS_PRESET = 2 };           // اقدام هنگام قطع ارتباط
enum ServerProto : uint8_t { PROTO_PLATFORM = 0, PROTO_GENERIC = 1 };                   // پلتفرم TiTiSwitch یا MQTT عمومی

static constexpr uint8_t MAX_SERVERS = 4;   // حداکثر سرور (به ترتیب امتحان می‌شوند)
static constexpr uint8_t MAX_NETS = 2;      // وای‌فای اصلی و پشتیبان

struct WifiNet {
  char ssid[33];
  char pass[65];
};

struct ServerCfg {
  bool enabled;
  uint8_t proto;          // ServerProto
  char name[24];
  char host[64];
  uint16_t port;
  bool tls;
  char user[64];          // پلتفرم: شناسهٔ دستگاه (device id)
  char pass[96];          // پلتفرم: رمز دستگاه (device secret)
  char clientId[48];      // خالی = از شناسهٔ دستگاه یا MAC ساخته می‌شود
  char prefix[64];        // پلتفرم: پیشوند تاپیک (معمولاً خالی)؛ عمومی: تاپیک پایه
  uint16_t keepalive;
  uint8_t qos;
};

// تنظیمات به صورت یک بلوک (blob) در NVS ذخیره می‌شود.
// ★ قاعدهٔ مهم: فیلد تازه فقط به انتهای ساختار اضافه شود (و عدد VERSION در settings.cpp بالا برود).
//   Firmware جدید بلوک قدیمی (کوتاه‌تر) را می‌خواند و فیلدهای تازه را با پیش‌فرض پر می‌کند،
//   پس آپدیت هیچ‌وقت تنظیمات و اتصال دستگاه را از بین نمی‌برد.
struct Settings {
  uint16_t version;
  char deviceName[32];
  uint8_t mode;           // LinkMode

  // --- Wi-Fi ---
  WifiNet nets[MAX_NETS];  // [0] اصلی، [1] پشتیبان
  bool dhcp;
  uint32_t ip, gateway, subnet, dns1, dns2;   // IPAddress به صورت عدد ۳۲ بیتی
  char hostname[32];
  char ntp[48];

  // --- سرورها (به ترتیب امتحان می‌شوند؛ اگر یکی وصل نشد نفر بعدی) ---
  ServerCfg servers[MAX_SERVERS];
  char deviceId[32];      // شناسهٔ دستگاه در پلتفرم؛ در تاپیک‌ها: d/<id>/...
  uint16_t telemetryS;    // هر چند ثانیه وضعیت ارسال شود
  char apiUrl[96];        // نشانی HTTP(S) هاب (کلید محلی، قواعد، آپدیت)، مثلاً https://hub.example.com
  bool localEnabled;      // کنترل از وای‌فای خانه (امضاشده؛ کلید از هاب می‌آید)
  bool otaEnabled;        // پذیرش آپدیت Firmware از پلتفرم

  // --- RS-485 / Modbus RTU Slave ---
  uint8_t netMode;        // NetMode: 0 خودکار (LAN اگر کابل وصل است، وگرنه وای‌فای)، 1 فقط وای‌فای، 2 فقط LAN
                          // (همان جای «آدرس از دیپ» قدیمی در Blob؛ مقدار قدیمی 0 = خودکار)
  uint8_t mbAddr;
  uint32_t mbBaud;
  uint8_t mbParity;       // 0 بدون، 1 زوج، 2 فرد
  uint8_t mbStopBits;     // 1 یا 2
  uint16_t mbRespDelayMs; // تأخیر پیش از جواب (برای Masterهای کند)
  bool mbSwapRxTx;        // جابه‌جایی پایه‌های RX و TX

  // --- قطع ارتباط (سکوت Master یا قطع MQTT) ---
  uint16_t linkTimeoutS;  // 0 = غیرفعال
  uint8_t linkLoss;       // LinkLoss
  uint8_t linkLossMask;   // برای LOSS_PRESET: کدام رله‌ها روشن شوند

  // --- رله‌ها ---
  uint8_t powerOn[RELAY_COUNT];       // PowerOn هر رله
  uint8_t interlock[RELAY_COUNT];     // رلهٔ شریک اینترلاک (۱..۸)، 0 = ندارد
  char relayName[RELAY_COUNT][20];
  uint16_t minIntervalMs;             // حداقل فاصلهٔ دو تغییر هر رله
  uint16_t staggerMs;                 // فاصلهٔ وصل شدن دو کویل

  // --- سیستم ---
  char apPass[65];        // رمز وای‌فای برد (حالت تنظیمات)
  char adminPass[33];     // رمز ورود به منو
  uint8_t logLevel;

  // --- نسخهٔ ۳ ---
  bool mbWordSwap;        // عددهای ۳۲ بیتی Modbus: اول کلمهٔ پایین (Low word first)

  // --- نسخهٔ ۴ ---
  uint16_t mbBase[4];     // آدرس شروع هر جدول روی سیم: [0] Coil [1] Discrete [2] Input [3] Holding
  bool mbLenient;         // خواندن بلوکی که بخشی از آن خارج از نقشه است: بخش خارج را صفر جواب بده
                          // (برای PLCهایی که یک‌جا چند رجیستر می‌خوانند). خطای 02 فقط وقتی کل درخواست خارج باشد.
};

// -------------------------------------------------------------------------------------
//  ۱۳) دسترسی به تنظیمات ذخیره‌شده (settings.cpp)
// -------------------------------------------------------------------------------------
namespace settings {
extern Settings cur;                       // تنظیمات فعلی (فقط خواندنی؛ برای تغییر از save استفاده کنید)
void defaults(Settings& s);                // مقادیر پیش‌فرض
void load();                               // خواندن از حافظه (بلوک قدیمی‌تر هم قابل قبول است)
bool save(const Settings& s);              // پیش از ذخیره اعتبارسنجی می‌کند؛ اگر نامعتبر بود چیزی ذخیره نمی‌شود
bool validate(Settings& s, String& err);   // خطا: نام فیلد نامعتبر در err
// بازگشت به تنظیمات کارخانه. هویت تولید (شناسه و رمز پلتفرم، سرور، نشانی هاب، CA و برچسب)
// می‌ماند، مگر wipeIdentity=true که دستگاه کاملاً خام می‌شود (فقط کنسول تولید).
void factoryReset(bool wipeIdentity = false);

// برچسب محصول از فایل CSV سامانهٔ تولید (QR، شمارهٔ سریال). جدا از تنظیمات نگه داشته می‌شود.
struct Label { String qr; String serial; };
Label label();
bool setLabel(const String& qr, const String& serial);

// گواهی CA سرور (PEM) برای اتصال امن؛ اگر ثبت نشده باشد فایل /ca.pem از حافظهٔ LittleFS و
// در نهایت گواهی‌های عمومی داخلی ESP32 استفاده می‌شود.
String caPem();
bool setCaPem(const String& pem);

uint8_t modbusAddress();                   // آدرس مؤثر Slave (دیپ یا دستی)؛ 0 = نامعتبر
int8_t platformServer();                   // اندیس اولین سرور پلتفرم فعال، -1 اگر نیست
const char* deviceSecret();                // رمز دستگاه (رمز همان سرور)
String apSsid();                           // نام وای‌فای برد: TiTi-R8-xxxx
String defaultClientId();
}
