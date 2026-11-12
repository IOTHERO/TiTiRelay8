// =====================================================================================
//  settings.cpp — ذخیره و بازیابی تنظیمات در حافظهٔ NVS
// =====================================================================================
//
//  ساختار Settings و مقدارهای پیش‌فرض در config.h هستند. این فایل:
//    - مقدار پیش‌فرض می‌سازد (defaults)، اعتبارسنجی می‌کند (validate)،
//    - تنظیمات را به صورت «یک بلوک نسخه‌دار» می‌نویسد و می‌خواند (save / load)،
//    - هویت دستگاه (شناسه و رمز پلتفرم، سرور، وای‌فای اصلی) را جدا هم نگه می‌دارد،
//    - برچسب محصول و گواهی CA را نگه می‌دارد.
//
//  ★ سازگاری با آپدیت: تنظیمات جدید فقط به انتهای struct Settings اضافه می‌شود. load() بلوک
//    قدیمی‌تر (کوتاه‌تر) را روی مقدارهای پیش‌فرض می‌خواند، پس فیلدهای تازه با پیش‌فرض پر
//    می‌شوند و تنظیمات قبلی از بین نمی‌رود.
//  ★ نسخهٔ ۴: آدرس شروع جدول‌های Modbus (mbBase) و حالت mbLenient.
// =====================================================================================
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <stddef.h>
#include "core.h"

namespace settings {
Settings cur;

namespace {
constexpr uint16_t VERSION = 4;           // با اضافه شدن فیلد به انتهای Settings یکی بالا ببرید
constexpr const char* NS = "cfg";         // فضای نام NVS برای بلوک تنظیمات
constexpr const char* IDENT = "ident";    // فضای نام NVS برای نسخهٔ پشتیبان هویت دستگاه
// پایان فیلدهای هر نسخهٔ قدیمی‌تر (برای خواندن بلوک ذخیره‌شده با Firmware قبلی):
//   نسخهٔ ۲ تا logLevel بود (پیش از mbWordSwap)، نسخهٔ ۳ تا mbWordSwap (پیش از mbBase).
constexpr size_t SIZE_V2 = offsetof(Settings, mbWordSwap);
constexpr size_t SIZE_V3 = offsetof(Settings, mbBase);

size_t endOf(uint16_t ver) { return ver == 2 ? SIZE_V2 : ver == 3 ? SIZE_V3 : sizeof(Settings); }

// سرعت‌های مجاز مدباس
const uint32_t BAUDS[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};

// هویت تولید (شناسه و رمز پلتفرم، آدرس هاب، سرور، وای‌فای اصلی و نوع ارتباط) علاوه بر بلوک
// اصلی در کلیدهای جدا هم ذخیره می‌شود. اگر بلوک اصلی روزی خراب یا ناخوانا شد، دستگاه باز هم
// به پلتفرم وصل می‌شود و نیازی به برنامه‌ریزی دوباره نیست.
void saveIdentity(const Settings& s) {
  Preferences p;
  if (!p.begin(IDENT, false)) return;
  p.putString("id", s.deviceId);
  p.putString("api", s.apiUrl);
  int8_t i = -1;
  for (uint8_t k = 0; k < MAX_SERVERS; k++)
    if (s.servers[k].enabled && s.servers[k].proto == PROTO_PLATFORM) { i = k; break; }
  if (i >= 0) p.putBytes("srv", &s.servers[i], sizeof(ServerCfg));
  else p.remove("srv");
  p.putUChar("mode", s.mode);
  p.putBytes("net", &s.nets[0], sizeof(WifiNet));
  p.end();
}

// withLink: نوع ارتباط و وای‌فای اصلی هم برگردانده شود (یعنی بلوک خراب شده، نه اینکه عمداً ریست شده).
bool restoreIdentity(Settings& s, bool withLink) {
  Preferences p;
  if (!p.begin(IDENT, true)) return false;
  bool any = false;
  String id = p.getString("id", "");
  if (id.length()) { strlcpy(s.deviceId, id.c_str(), sizeof s.deviceId); any = true; }
  strlcpy(s.apiUrl, p.getString("api", "").c_str(), sizeof s.apiUrl);
  ServerCfg v;
  if (p.getBytesLength("srv") == sizeof v && p.getBytes("srv", &v, sizeof v) == sizeof v) {
    // رشته‌ها را حتماً پایان‌دار کن (حتی اگر داده خراب باشد)
    v.name[sizeof v.name - 1] = v.host[sizeof v.host - 1] = v.user[sizeof v.user - 1] = 0;
    v.pass[sizeof v.pass - 1] = v.clientId[sizeof v.clientId - 1] = v.prefix[sizeof v.prefix - 1] = 0;
    s.servers[0] = v;
    any = true;
  }
  if (withLink) {
    WifiNet n;
    if (p.getBytesLength("net") == sizeof n && p.getBytes("net", &n, sizeof n) == sizeof n) {
      n.ssid[sizeof n.ssid - 1] = n.pass[sizeof n.pass - 1] = 0;
      s.nets[0] = n;
    }
    uint8_t m = p.getUChar("mode", MODE_UNSET);
    if (m <= MODE_RS485) s.mode = m;
  }
  p.end();
  return any;
}

// چهار رقم آخر MAC (برای نام وای‌فای برد و Client ID)
String macSuffix() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);   // پیش از روشن شدن Wi-Fi هم معتبر است
  char b[5];
  snprintf(b, sizeof b, "%02X%02X", mac[4], mac[5]);
  return String(b);
}

bool printable(const char* s) {
  for (; *s; s++) if ((uint8_t)*s < 0x20) return false;
  return true;
}

// تاپیک MQTT نباید علامت‌های wildcard (+ و #) یا کاراکتر کنترلی داشته باشد
bool topicSafe(const char* s) {
  for (; *s; s++) if (*s == '+' || *s == '#' || (uint8_t)*s < 0x20) return false;
  return true;
}
}  // namespace

// -------------------------------------------------------------------------------------
//  مقدار پیش‌فرض (پس از ریست کارخانه یا برای فیلدهای تازهٔ یک نسخهٔ جدید)
// -------------------------------------------------------------------------------------
void defaults(Settings& s) {
  memset(&s, 0, sizeof s);
  s.version = VERSION;
  strlcpy(s.deviceName, FW_NAME, sizeof s.deviceName);
  s.mode = MODE_UNSET;
  s.dhcp = true;
  s.subnet = (uint32_t)IPAddress(255, 255, 255, 0);
  snprintf(s.hostname, sizeof s.hostname, "titi-r8-%s", macSuffix().c_str());
  for (char* p = s.hostname; *p; p++) *p = tolower(*p);
  strlcpy(s.ntp, DEFAULT_NTP_SERVER, sizeof s.ntp);
  for (uint8_t i = 0; i < MAX_SERVERS; i++) {
    ServerCfg& v = s.servers[i];
    v.enabled = false;
    v.proto = PROTO_PLATFORM;
    snprintf(v.name, sizeof v.name, "Server %u", i + 1);
    v.port = DEFAULT_MQTT_PORT;
    v.keepalive = DEFAULT_MQTT_KEEPALIVE_S;
    v.qos = DEFAULT_MQTT_QOS;
  }
  s.telemetryS = DEFAULT_TELEMETRY_S;
  s.localEnabled = true;
  s.otaEnabled = true;
  s.netMode = NET_AUTO;  // LAN اگر کابل وصل است، وگرنه وای‌فای
  s.mbAddr = DEFAULT_MB_ADDR;
  s.mbBaud = DEFAULT_MB_BAUD;
  s.mbParity = DEFAULT_MB_PARITY;
  s.mbStopBits = DEFAULT_MB_STOP_BITS;
  s.mbRespDelayMs = 0;
  s.mbSwapRxTx = false;
  s.mbWordSwap = false;
  for (uint8_t t = 0; t < 4; t++) s.mbBase[t] = 0;    // آدرس شروع هر جدول: استاندارد (از صفر)
  s.mbLenient = false;
  s.linkTimeoutS = DEFAULT_LINK_TIMEOUT_S;
  s.linkLoss = LOSS_HOLD;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    s.powerOn[i] = PON_RESTORE;
    snprintf(s.relayName[i], sizeof s.relayName[i], "Relay %u", i + 1);
  }
  s.minIntervalMs = DEFAULT_MIN_INTERVAL_MS;
  s.staggerMs = DEFAULT_STAGGER_MS;
  strlcpy(s.apPass, DEFAULT_AP_PASSWORD, sizeof s.apPass);
  strlcpy(s.adminPass, DEFAULT_ADMIN_PASSWORD, sizeof s.adminPass);
  s.logLevel = DEFAULT_LOG_LEVEL;
}

// -------------------------------------------------------------------------------------
//  اعتبارسنجی: هر مقدار نامعتبر → false و نام فیلد در err (منو آن را به کاربر نشان می‌دهد)
// -------------------------------------------------------------------------------------
bool validate(Settings& s, String& err) {
  // رشته‌ها حتماً پایان‌دار شوند (حتی اگر بلوک ذخیره‌شده خراب باشد)
  s.deviceName[sizeof s.deviceName - 1] = 0;
  s.hostname[sizeof s.hostname - 1] = 0;
  s.deviceId[sizeof s.deviceId - 1] = 0;
  if (s.mode > MODE_RS485) { err = "mode"; return false; }
  if (!printable(s.deviceName)) { err = "deviceName"; return false; }
  if (s.mode == MODE_WIFI) {
    // با کابل LAN وای‌فای لازم نیست؛ «فقط وای‌فای» (یا برد بدون W5500) شبکهٔ وای‌فای می‌خواهد
    if (!s.nets[0].ssid[0] && (s.netMode == NET_WIFI || !HAS_LAN)) {
      err = "wifi.ssid";
      return false;
    }
    bool any = false;
    for (uint8_t i = 0; i < MAX_SERVERS; i++) {
      ServerCfg& v = s.servers[i];
      if (!v.enabled) continue;
      any = true;
      if (!v.host[0] || !printable(v.host)) { err = "servers." + String(i) + ".host"; return false; }
      if (v.port == 0) { err = "servers." + String(i) + ".port"; return false; }
      if (v.qos > 1) { err = "servers." + String(i) + ".qos"; return false; }
      if (v.keepalive < 5 || v.keepalive > 600) { err = "servers." + String(i) + ".keepalive"; return false; }
      if (!topicSafe(v.prefix)) { err = "servers." + String(i) + ".prefix"; return false; }
      if (v.proto == PROTO_PLATFORM && !s.deviceId[0]) { err = "deviceId"; return false; }
      if (v.proto == PROTO_GENERIC && !v.prefix[0]) { err = "servers." + String(i) + ".prefix"; return false; }
    }
    if (!any) { err = "servers"; return false; }
    if (!s.dhcp && ((uint32_t)s.ip == 0 || (uint32_t)s.gateway == 0)) { err = "wifi.ip"; return false; }
  }
  if (!topicSafe(s.deviceId)) { err = "deviceId"; return false; }
  s.apiUrl[sizeof s.apiUrl - 1] = 0;
  if (s.apiUrl[0] && strncmp(s.apiUrl, "https://", 8) != 0 && strncmp(s.apiUrl, "http://", 7) != 0) {
    err = "apiUrl";
    return false;
  }
  size_t au = strlen(s.apiUrl);
  if (au && s.apiUrl[au - 1] == '/') s.apiUrl[au - 1] = 0;     // اسلش انتهای نشانی حذف شود
  if (s.telemetryS < 5 || s.telemetryS > 3600) { err = "telemetryS"; return false; }
  if (s.netMode > NET_LAN) { err = "netMode"; return false; }

  // --- Modbus ---
  bool baudOk = false;
  for (uint32_t b : BAUDS) baudOk |= b == s.mbBaud;
  if (!baudOk) { err = "mb.baud"; return false; }
  if (s.mbAddr < 1 || s.mbAddr > 247) { err = "mb.addr"; return false; }
  if (s.mbParity > 2 || s.mbStopBits < 1 || s.mbStopBits > 2) { err = "mb.format"; return false; }
  if (s.mbRespDelayMs > 500) { err = "mb.respDelay"; return false; }
  // آدرس شروع هر جدول: آخرین رجیستر جدول هم باید در بازهٔ ۱۶ بیتی (تا 65535) بماند
  for (uint8_t t = 0; t < 4; t++)
    if ((uint32_t)s.mbBase[t] + MB_TABLE_LEN[t] > 65536UL) { err = "mb.base"; return false; }

  if (s.linkTimeoutS > 3600 || s.linkLoss > LOSS_PRESET) { err = "linkLoss"; return false; }
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    s.relayName[i][sizeof s.relayName[i] - 1] = 0;
    if (s.powerOn[i] > PON_ON) { err = "relays.powerOn"; return false; }
    uint8_t p = s.interlock[i];
    if (p > RELAY_COUNT || p == i + 1) { err = "relays.interlock"; return false; }
    // اینترلاک دوطرفه است: رلهٔ شریک باید به این رله برگردد
    if (p && s.interlock[p - 1] != i + 1) { err = "relays.interlock"; return false; }
  }
  if (s.minIntervalMs > 10000 || s.staggerMs > 1000) { err = "relays.timing"; return false; }
  size_t ap = strlen(s.apPass);
  if (ap < 8 || ap > 63) { err = "apPass"; return false; }     // رمز WPA2 باید ۸ تا ۶۳ حرف باشد
  if (strlen(s.adminPass) < 4) { err = "adminPass"; return false; }
  if (s.logLevel > LOG_DEBUG) s.logLevel = LOG_INFO;
  return true;
}

// -------------------------------------------------------------------------------------
//  خواندن از حافظه
//  - بلوکی با همان اندازه یا کوتاه‌تر (از نسخهٔ قدیمی‌تر) روی مقدارهای پیش‌فرض خوانده می‌شود؛
//    فیلدهای تازه پیش‌فرض می‌مانند.
//  - بلوک بزرگ‌تر (برنگشتن به Firmware قدیمی‌تر) خوانده نمی‌شود.
//  - اگر چیزی نبود (بعد از ریست کارخانه): فقط هویت برمی‌گردد. اگر بلوک ناخوانا بود: هویت و
//    اتصال (نوع ارتباط و وای‌فای) هم از کلیدهای پشتیبان برمی‌گردد.
// -------------------------------------------------------------------------------------
void load() {
  defaults(cur);
  Preferences p;
  size_t n = 0;
  Settings* tmp = new Settings;
  defaults(*tmp);
  bool ok = false;
  if (p.begin(NS, true)) {
    n = p.getBytesLength("s");
    if (n >= SIZE_V2 && n <= sizeof(Settings)) {
      // بلوک را اول در یک بافر خام می‌خوانیم، سپس فقط «فیلدهای همان نسخه» را روی پیش‌فرض‌ها
      // کپی می‌کنیم. بایت‌های اضافهٔ انتهای بلوک قدیمی (پدینگ ساختار) هرگز وارد فیلدهای تازه
      // نمی‌شوند؛ فیلدهای تازه همان پیش‌فرض می‌مانند.
      uint8_t* raw = (uint8_t*)malloc(n);
      if (raw && p.getBytes("s", raw, n) == n) {
        uint16_t ver;
        memcpy(&ver, raw, sizeof ver);                 // version اولین فیلد ساختار است
        if (ver >= 2 && ver <= VERSION && n >= endOf(ver)) {
          memcpy(tmp, raw, endOf(ver) < n ? endOf(ver) : n);
          if (ver < VERSION) LOGI("cfg", "settings from version %u upgraded", ver);
          tmp->version = VERSION;
          ok = true;
        }
      }
      free(raw);
    }
    p.end();
  }
  String err;
  if (ok && validate(*tmp, err)) {
    cur = *tmp;
    LOGI("cfg", "settings loaded");
  } else {
    bool lost = n > 0;          // بلوکی بود ولی خوانده نشد
    if (lost) LOGE("cfg", "stored settings unreadable (%s)", err.length() ? err.c_str() : "layout");
    if (restoreIdentity(cur, lost)) {
      String e2;
      Settings check = cur;
      if (!validate(check, e2)) { cur.mode = MODE_UNSET; }
      LOGW("cfg", lost ? "identity and link restored from the backup keys" : "identity restored");
    }
  }
  delete tmp;
}

bool save(const Settings& in) {
  Settings s = in;
  String err;
  if (!validate(s, err)) {
    LOGW("cfg", "refused settings: %s", err.c_str());
    return false;
  }
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.putBytes("s", &s, sizeof s) == sizeof s;
  p.end();
  if (ok) {
    cur = s;
    saveIdentity(s);
    LOGI("cfg", "settings saved");
  } else {
    LOGE("cfg", "settings write failed");
  }
  return ok;
}

// -------------------------------------------------------------------------------------
//  ریست کارخانه
//  wipeIdentity=false (ریست مشتری از منو یا با کلید کانفیگ): تنظیمات پاک می‌شود ولی هویت دستگاه،
//     گواهی CA و برچسب می‌ماند (load() هویت را برمی‌گرداند)؛ وای‌فای مشتری پاک می‌شود.
//  wipeIdentity=true (کنسول خط تولید): همه‌چیز پاک می‌شود تا برد دوباره برنامه‌ریزی شود.
// -------------------------------------------------------------------------------------
void factoryReset(bool wipeIdentity) {
  Preferences p;
  if (p.begin(NS, false)) {
    String ca = p.getString("ca", "");
    p.clear();
    if (!wipeIdentity && ca.length()) p.putString("ca", ca);
    p.end();
  }
  if (p.begin(IDENT, false)) {
    if (wipeIdentity) p.clear();
    else { p.remove("net"); p.remove("mode"); }
    p.end();
  }
  // وضعیت رله‌ها، کلید کنترل محلی، قواعد آفلاین و نتیجهٔ آپدیت
  for (const char* ns : {"rt", "loc", "rules", "ota"})
    if (p.begin(ns, false)) { p.clear(); p.end(); }
  if (wipeIdentity && p.begin("label", false)) { p.clear(); p.end(); }
  LOGW("cfg", wipeIdentity ? "erased, identity included" : "factory reset (identity kept)");
}

// -------------------------------------------------------------------------------------
//  برچسب محصول (QR و شمارهٔ سریال از CSV سامانهٔ تولید) — جدا از تنظیمات، پس از ریست می‌ماند
// -------------------------------------------------------------------------------------
Label label() {
  Label l;
  Preferences p;
  if (p.begin("label", true)) {
    l.qr = p.getString("qr", "");
    l.serial = p.getString("sn", "");
    p.end();
  }
  return l;
}

bool setLabel(const String& qr, const String& serial) {
  if (qr.length() > 200 || serial.length() > 40) return false;
  if (qr.length() && !qr.startsWith("https://") && !qr.startsWith("http://")) return false;
  for (char c : qr) if ((uint8_t)c < 0x21) return false;      // فاصله و کاراکتر کنترلی ممنوع
  Preferences p;
  if (!p.begin("label", false)) return false;
  if (qr.length()) p.putString("qr", qr); else p.remove("qr");
  if (serial.length()) p.putString("sn", serial); else p.remove("sn");
  p.end();
  LOGI("cfg", "product label %s", qr.length() ? "stored" : "cleared");
  return true;
}

// -------------------------------------------------------------------------------------
//  گواهی CA سرور برای اتصال امن (TLS با CA خصوصی)
//  اولویت: گواهی واردشده در منو (NVS) ← فایل /ca.pem در LittleFS (داخل Release ساخته می‌شود) ←
//  (خالی) یعنی گواهی‌های عمومی داخلی ESP32.
// -------------------------------------------------------------------------------------
String caPem() {
  Preferences p;
  String v;
  if (p.begin(NS, true)) {
    v = p.getString("ca", "");
    p.end();
  }
  if (v.length()) return v;
  if (LittleFS.begin(false) && LittleFS.exists("/ca.pem")) {
    File f = LittleFS.open("/ca.pem", "r");
    if (f && f.size() > 0 && f.size() <= 4000) v = f.readString();
  }
  return v;
}

bool setCaPem(const String& pem) {
  if (pem.length() > 4000) return false;
  if (pem.length() && pem.indexOf("-----BEGIN CERTIFICATE-----") < 0) return false;
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = true;
  if (pem.length()) ok = p.putString("ca", pem) > 0;
  else p.remove("ca");
  p.end();
  return ok;
}

// -------------------------------------------------------------------------------------
//  توابع کمکی
// -------------------------------------------------------------------------------------
// آدرس Slave در Modbus RTU فقط از منو (۱ تا ۲۴۷)؛ دیپ‌سوئیچ آدرس دیگر روی برد نیست. این آدرس فقط
// برای RS-485 است: در شبکه (وای‌فای یا LAN) هر برد با شناسهٔ یکتای خودش (device_id) شناخته می‌شود و
// دو برد با آدرس یکسان تداخلی ندارند.
uint8_t modbusAddress() {
  return cur.mbAddr >= 1 && cur.mbAddr <= 247 ? cur.mbAddr : 0;
}

int8_t platformServer() {
  for (uint8_t i = 0; i < MAX_SERVERS; i++)
    if (cur.servers[i].enabled && cur.servers[i].proto == PROTO_PLATFORM) return i;
  return -1;
}

const char* deviceSecret() {
  int8_t i = platformServer();
  return i < 0 ? "" : cur.servers[i].pass;
}

String apSsid() { return String(PORTAL_AP_SSID_PREFIX) + macSuffix(); }

String defaultClientId() {
  return cur.deviceId[0] ? String(cur.deviceId) : String("titi-r8-") + macSuffix();
}
}  // namespace settings
