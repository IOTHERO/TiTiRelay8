// =====================================================================================
//  net.cpp — شبکه: LAN (ماژول W5500) و وای‌فای (Station)، و ساعت
// =====================================================================================
//
//  حالت شبکه (settings.netMode):
//    خودکار (پیش‌فرض) : اگر کابل LAN وصل است از LAN، وگرنه وای‌فای. اگر کابل بعداً وصل یا قطع شود
//                      همان لحظه جابه‌جا می‌شود. هر دو هم‌زمان استفاده نمی‌شوند (IP ثابت یکی است).
//    فقط وای‌فای / فقط LAN.
//  وای‌فای: شبکهٔ اصلی و پشتیبان، با فاصلهٔ افزایشی (۵ ثانیه تا ۱ دقیقه، با کمی تصادفی) و جابه‌جایی
//  بین دو شبکه. IP ثابت یا DHCP برای هر کدام که فعال است. پس از اتصال، ساعت از NTP گرفته می‌شود؛
//  بدون اینترنت (پشت هاب) ساعت را هاب با پیام config از نوع time می‌دهد (local.cpp).
//  MQTT (esp-mqtt) و وب‌سرور محلی روی هر دو رابط کار می‌کنند؛ فقط رابط فعال عوض می‌شود.
// =====================================================================================
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>
#include "core.h"
#if HAS_LAN
#include <ETH.h>
#endif

namespace net {
namespace {
enum State : uint8_t { OFF, WAIT, CONNECTING, UP };
State state = OFF;            // وای‌فای
uint8_t netIdx = 0;
uint32_t since = 0;
uint32_t nextAt = 0;
uint8_t failures = 0;
bool ntpStarted = false;
bool apToo = false;
bool running = false;
const char* statusText = "off";

volatile bool ethLink = false, ethIp = false;
bool ethStarted = false;
uint32_t lanWaitUntil = 0;    // حالت خودکار: تا این لحظه منتظر کابل LAN می‌مانیم

bool wantLan() { return HAS_LAN && settings::cur.netMode != NET_WIFI; }
bool wantWifi() {
  if (settings::cur.netMode == NET_LAN && HAS_LAN) return false;
  if (settings::cur.netMode == NET_WIFI || !HAS_LAN || !ethStarted) return true;
  return !ethIp && (int32_t)(millis() - lanWaitUntil) >= 0;    // خودکار: LAN آدرس ندارد
}

void startNtp() {
  if (!ntpStarted && settings::cur.ntp[0]) {
    configTime(0, 0, settings::cur.ntp, NTP_FALLBACK_1, NTP_FALLBACK_2);
    ntpStarted = true;
  }
}

#if HAS_LAN
void onEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START: ETH.setHostname(settings::cur.hostname); break;
    case ARDUINO_EVENT_ETH_CONNECTED: ethLink = true; break;
    case ARDUINO_EVENT_ETH_GOT_IP: ethIp = true; break;
    case ARDUINO_EVENT_ETH_LOST_IP: ethIp = false; break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: ethLink = false; ethIp = false; break;
    default: break;
  }
}

void startLan() {
  if (ethStarted || !wantLan()) return;
  const Settings& s = settings::cur;
  if (PIN_ETH_RST >= 0) {                         // ریست تمیز تراشه، اگر سیمش روی برد هست
    pinMode(PIN_ETH_RST, OUTPUT);
    digitalWrite(PIN_ETH_RST, LOW);
    delay(10);
    digitalWrite(PIN_ETH_RST, HIGH);
    delay(50);
  }
  ethStarted = ETH.begin(ETH_PHY_W5500, 1, PIN_ETH_CS, PIN_ETH_INT, PIN_ETH_RST, SPI3_HOST,
                         PIN_ETH_SCK, PIN_ETH_MISO, PIN_ETH_MOSI);
  if (!ethStarted) {
    LOGE("lan", "W5500 did not start (not fitted or wiring): Wi-Fi only");
    return;
  }
  if (!s.dhcp) {
    ETH.config(IPAddress(s.ip), IPAddress(s.gateway), IPAddress(s.subnet), IPAddress(s.dns1),
               IPAddress(s.dns2));
  }
  LOGI("lan", "W5500 started");
}
#endif

void startWifi() {
  const Settings& s = settings::cur;
  if (!s.nets[netIdx].ssid[0]) netIdx = 0;
  const WifiNet& n = s.nets[netIdx];
  if (!n.ssid[0]) {                               // وای‌فای تنظیم نشده (مثلاً فقط کابل)
    state = WAIT;
    nextAt = millis() + WIFI_BACKOFF_MAX_MS;
    statusText = "no-wifi";
    return;
  }
  if (!s.dhcp) {
    WiFi.config(IPAddress(s.ip), IPAddress(s.gateway), IPAddress(s.subnet), IPAddress(s.dns1),
                IPAddress(s.dns2));
  }
  WiFi.begin(n.ssid, n.pass[0] ? n.pass : nullptr);
  state = CONNECTING;
  since = millis();
  statusText = "connecting";
  LOGI("wifi", "connecting to \"%s\"%s", n.ssid, netIdx ? " (backup)" : "");
}

void stopWifi() {
  if (state == OFF) return;
  WiFi.disconnect(false, false);
  state = OFF;
  statusText = "off";
}

void retryLater() {
  WiFi.disconnect(false, false);
  failures++;
  // فاصلهٔ تلاش بعدی: ۵ ثانیه، ۱۰، ۲۰، ... تا سقف یک دقیقه، به‌علاوهٔ مقدار تصادفی
  uint32_t delayMs = WIFI_BACKOFF_MIN_MS << (failures > 4 ? 4 : failures - 1);
  if (delayMs > WIFI_BACKOFF_MAX_MS) delayMs = WIFI_BACKOFF_MAX_MS;
  delayMs += esp_random() % WIFI_BACKOFF_JITTER_MS;
  // پس از هر شکست، به شبکهٔ دیگر (اصلی ↔ پشتیبان) برو.
  if (settings::cur.nets[1].ssid[0]) netIdx ^= 1;
  nextAt = millis() + delayMs;
  state = WAIT;
  statusText = "retry";
  LOGW("wifi", "not connected, next try in %lu s", (unsigned long)(delayMs / 1000));
}

void wifiLoop() {
  uint32_t now = millis();
  switch (state) {
    case OFF:
      startWifi();
      return;
    case WAIT:
      if ((int32_t)(now - nextAt) >= 0) startWifi();
      return;
    case CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        state = UP;
        failures = 0;
        statusText = "connected";
        LOGI("wifi", "connected: %s, rssi %d dBm, ch %d", WiFi.localIP().toString().c_str(),
             WiFi.RSSI(), WiFi.channel());
        startNtp();
      } else if (now - since > WIFI_CONNECT_TIMEOUT_MS) {
        retryLater();
      }
      return;
    case UP:
      if (WiFi.status() != WL_CONNECTED) {
        LOGW("wifi", "connection lost");
        failures = 0;
        retryLater();
      }
      return;
  }
}

bool lastIp = false, lastLink = false;
}  // namespace

void begin(bool keepAp) {
  apToo = keepAp;
  running = true;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(keepAp ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(settings::cur.hostname);
  WiFi.setSleep(false);
  netIdx = 0;
  failures = 0;
  state = OFF;
  lanWaitUntil = millis() + (settings::cur.netMode == NET_AUTO ? LAN_LINK_WAIT_MS : 0);
#if HAS_LAN
  static bool hooked = false;
  if (!hooked) {
    WiFi.onEvent(onEvent);
    hooked = true;
  }
  startLan();
#endif
}

void stop() {
  running = false;
  stopWifi();
  if (!apToo) WiFi.mode(WIFI_OFF);
}

void loop() {
  if (!running) return;
  if (ethLink != lastLink) {                      // پیام‌ها اینجا (نه در رویداد شبکه) نوشته می‌شوند
    lastLink = ethLink;
    LOGI("lan", "cable %s", lastLink ? "connected" : "disconnected");
  }
  if (ethIp != lastIp) {
    lastIp = ethIp;
    if (lastIp) {
      LOGI("lan", "connected: %s", ip().c_str());
      startNtp();
    }
  }
  if (wantWifi()) {
    wifiLoop();
  } else if (state != OFF) {
    LOGI("wifi", "LAN is up: Wi-Fi station off");
    stopWifi();
  }
}

bool lan() { return ethIp && state != UP; }
bool lanLink() { return ethLink; }
bool up() { return ethIp || state == UP; }
int rssi() { return state == UP ? WiFi.RSSI() : 0; }
String ip() {
#if HAS_LAN
  if (lan()) return ETH.localIP().toString();
#endif
  return state == UP ? WiFi.localIP().toString() : String();
}
String ssid() {
  if (lan()) return String("LAN");
  return state == UP ? WiFi.SSID() : String();
}
String mac() {
#if HAS_LAN
  if (lan()) return ETH.macAddress();
#endif
  return WiFi.macAddress();
}
const char* status() { return lan() ? "lan" : statusText; }

bool timeValid() { return time(nullptr) > 1700000000; }

uint64_t epochMs() {
  if (!timeValid()) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}
}  // namespace net
