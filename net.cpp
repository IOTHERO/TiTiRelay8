// =====================================================================================
//  net.cpp — اتصال وای‌فای (Station) و ساعت
// =====================================================================================
//
//  وای‌فای اصلی و پشتیبان، IP ثابت یا DHCP. اگر اتصال برقرار نشود یا قطع شود، با فاصلهٔ
//  افزایشی (۵ ثانیه تا ۱ دقیقه، با کمی تصادفی) دوباره تلاش می‌کند و بین شبکهٔ اصلی و پشتیبان
//  جابه‌جا می‌شود. پس از اتصال، ساعت از سرور NTP گرفته می‌شود (TLS و قواعد زمانی به آن نیاز دارند).
//  همهٔ زمان‌ها و سرورهای زمان پشتیبان در config.h هستند.
// =====================================================================================
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>
#include "core.h"

namespace net {
namespace {
enum State : uint8_t { OFF, WAIT, CONNECTING, UP };
State state = OFF;
uint8_t netIdx = 0;
uint32_t since = 0;
uint32_t nextAt = 0;
uint8_t failures = 0;
bool ntpStarted = false;
bool apToo = false;
const char* statusText = "off";

void start() {
  const Settings& s = settings::cur;
  if (!s.nets[netIdx].ssid[0]) netIdx = 0;
  const WifiNet& n = s.nets[netIdx];
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
}  // namespace

void begin(bool keepAp) {
  apToo = keepAp;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(keepAp ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(settings::cur.hostname);
  WiFi.setSleep(false);
  netIdx = 0;
  failures = 0;
  start();
}

void stop() {
  if (state == OFF) return;
  WiFi.disconnect(false, false);
  if (!apToo) WiFi.mode(WIFI_OFF);
  state = OFF;
  statusText = "off";
}

void loop() {
  uint32_t now = millis();
  switch (state) {
    case OFF: return;
    case WAIT:
      if ((int32_t)(now - nextAt) >= 0) start();
      return;
    case CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        state = UP;
        failures = 0;
        statusText = "connected";
        LOGI("wifi", "connected: %s, rssi %d dBm, ch %d", WiFi.localIP().toString().c_str(),
             WiFi.RSSI(), WiFi.channel());
        if (!ntpStarted && settings::cur.ntp[0]) {
          configTime(0, 0, settings::cur.ntp, NTP_FALLBACK_1, NTP_FALLBACK_2);
          ntpStarted = true;
        }
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

bool up() { return state == UP; }
int rssi() { return state == UP ? WiFi.RSSI() : 0; }
String ip() { return state == UP ? WiFi.localIP().toString() : String(); }
String ssid() { return state == UP ? WiFi.SSID() : String(); }
const char* status() { return statusText; }

bool timeValid() { return time(nullptr) > 1700000000; }

uint64_t epochMs() {
  if (!timeValid()) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}
}  // namespace net
