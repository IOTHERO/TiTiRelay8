// =====================================================================================
//  local.cpp — کنترل محلی از وای‌فای خانه
// =====================================================================================
//
//  وقتی اینترنت (و سرور) در دسترس نیست ولی گوشی و برد در یک شبکهٔ خانه هستند، اپ فرمان را
//  مستقیم به برد می‌فرستد. برای اینکه کسی در همان شبکه نتواند فرمان جعلی بدهد:
//    ۱) کلید محلی (۳۲ بایت) را فقط هاب به برد و به گوشیِ مجاز می‌دهد (پیام config).
//    ۲) اپ اول یک «nonce» یک‌بارمصرف می‌گیرد (۳۰ ثانیه اعتبار).
//    ۳) فرمان با امضای HMAC-SHA256 روی «شناسه، nonce، نوع، تگ، مقدار» فرستاده می‌شود؛ فرمان
//       ضبط‌شده دوباره قابل ارسال نیست و بدون کلید هیچ کاری ممکن نیست.
//  برد خودش را با mDNS (_titiswitch._tcp) معرفی می‌کند و آنچه محلی انجام شده را بعداً در
//  local_log به سرور گزارش می‌دهد.
// =====================================================================================
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <time.h>
#include "core.h"

namespace local {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t key[32];
size_t keyLen = 0;
uint32_t keyVersion = 0;
uint16_t listenPort = LOCAL_DEFAULT_PORT;
volatile bool keyChanged = false;

AsyncWebServer* server = nullptr;
bool mdnsOn = false;
String lastIp;

struct Nonce { char v[24]; uint32_t at; };
Nonce nonces[LOCAL_MAX_NONCES] = {};

struct LogEntry { uint64_t atMs; uint8_t relay; bool value; char by[24]; };
LogEntry logRing[LOCAL_LOG_ENTRIES];
uint8_t logCount = 0;
SemaphoreHandle_t logMtx = nullptr;

size_t b64urlDecode(const char* in, uint8_t* out, size_t cap) {
  char buf[64];
  size_t n = strlen(in);
  if (n + 4 >= sizeof buf) return 0;
  for (size_t i = 0; i < n; i++) buf[i] = in[i] == '-' ? '+' : in[i] == '_' ? '/' : in[i];
  while (n % 4) buf[n++] = '=';
  buf[n] = 0;
  size_t olen = 0;
  if (mbedtls_base64_decode(out, cap, &olen, (const uint8_t*)buf, n) != 0) return 0;
  return olen;
}

void b64urlEncode(const uint8_t* in, size_t len, char* out, size_t cap) {
  size_t olen = 0;
  mbedtls_base64_encode((uint8_t*)out, cap, &olen, in, len);
  out[olen] = 0;
  for (char* p = out; *p; p++) {
    if (*p == '+') *p = '-';
    else if (*p == '/') *p = '_';
    else if (*p == '=') { *p = 0; break; }
  }
}

// بررسی امضا: HMAC-SHA256 پیام با کلید محلی، به‌صورت base64url، با مقایسهٔ زمان‌ثابت
// (تا از طول زمان پاسخ نتوان امضا را حدس زد).
bool verify(const String& msg, const char* sig) {
  uint8_t k[32];
  size_t kl;
  portENTER_CRITICAL(&mux);
  memcpy(k, key, sizeof k);
  kl = keyLen;
  portEXIT_CRITICAL(&mux);
  if (!kl || !sig) return false;
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), k, kl, (const uint8_t*)msg.c_str(),
                  msg.length(), mac);
  char expect[48];
  b64urlEncode(mac, sizeof mac, expect, sizeof expect);
  size_t n = strlen(expect);
  if (strlen(sig) != n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= expect[i] ^ sig[i];   // constant time
  return diff == 0;
}

// nonce فقط یک بار و تا LOCAL_NONCE_TTL_MS معتبر است؛ بلافاصله پس از مصرف باطل می‌شود.
bool takeNonce(const char* n) {
  uint32_t now = millis();
  for (Nonce& x : nonces) {
    if (x.v[0] && now - x.at < LOCAL_NONCE_TTL_MS && !strcmp(x.v, n)) {
      x.v[0] = 0;                       // one use only
      return true;
    }
  }
  return false;
}

void answer(AsyncWebServerRequest* r, int code, JsonDocument& d) {
  String s;
  serializeJson(d, s);
  r->send(code, "application/json", s);
}

void fail(AsyncWebServerRequest* r, int code, const char* error) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = error;
  answer(r, code, d);
}

void values(JsonObject v) {
  uint8_t st = relays::state();
  char k[10];
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (!relays::fitted(i)) continue;
    snprintf(k, sizeof k, "relay_%u", i + 1);
    v[k] = (bool)((st >> i) & 1);
  }
}

// Collects a POST body (any content type) into the request's temp object.
void onBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > LOCAL_MAX_BODY) return;
  if (index == 0) {
    r->_tempObject = calloc(total + 1, 1);
  }
  if (r->_tempObject && index + len <= total) memcpy((char*)r->_tempObject + index, data, len);
}

bool body(AsyncWebServerRequest* r, JsonDocument& d) {
  return r->_tempObject && !deserializeJson(d, (const char*)r->_tempObject);
}

void handleCommand(AsyncWebServerRequest* r) {
  JsonDocument b;
  if (!body(r, b)) return fail(r, 400, "bad request");
  if (!keySet()) return fail(r, 403, "local control is not set up");
  const char* nonce = b["nonce"] | "";
  if (!takeNonce(nonce)) return fail(r, 401, "nonce");
  const char* tag = b["tag"] | "";
  const char* vj = b["value_json"] | "";
  String msg = String(settings::cur.deviceId) + "\n" + nonce + "\ncommand\n" + tag + "\n" + vj;
  if (!verify(msg, b["sig"] | "")) {
    LOGW("local", "command with a wrong signature refused");
    return fail(r, 401, "signature");
  }
  int relay = -1;
  if (!strncmp(tag, "relay_", 6)) relay = atoi(tag + 6) - 1;
  if (relay < 0 || relay >= RELAY_COUNT || !relays::fitted(relay)) return fail(r, 200, "unknown tag");
  JsonDocument v;
  if (deserializeJson(v, vj)) return fail(r, 200, "bad value");
  bool on;
  if (v.is<bool>()) on = v.as<bool>();
  else if (v.is<long>() && (v.as<long>() == 0 || v.as<long>() == 1)) on = v.as<long>();
  else return fail(r, 200, "bad value");
  if (!relays::set(relay, on, relays::SRC_LOCAL)) return fail(r, 200, "busy");
  log(relay, on, "local");
  JsonDocument d;
  d["ok"] = true;
  d["tag"] = tag;
  d["value"] = on;
  answer(r, 200, d);
}

void handleState(AsyncWebServerRequest* r) {
  JsonDocument b;
  if (!body(r, b)) return fail(r, 400, "bad request");
  if (!keySet()) return fail(r, 403, "local control is not set up");
  const char* nonce = b["nonce"] | "";
  if (!takeNonce(nonce)) return fail(r, 401, "nonce");
  String msg = String(settings::cur.deviceId) + "\n" + nonce + "\nstate";
  if (!verify(msg, b["sig"] | "")) return fail(r, 401, "signature");
  JsonDocument d;
  d["ok"] = true;
  values(d["values"].to<JsonObject>());
  answer(r, 200, d);
}

void startServer() {
  server = new AsyncWebServer(listenPort);
  server->on("/local/v1/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    d["device_id"] = settings::cur.deviceId;
    d["firmware"] = FW_VERSION;
    d["local"] = keySet();
    answer(r, 200, d);
  });
  server->on("/local/v1/nonce", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!keySet()) return fail(r, 403, "local control is not set up");
    uint32_t now = millis();
    Nonce* slot = nullptr;
    for (Nonce& x : nonces) {
      if (x.v[0] && now - x.at >= LOCAL_NONCE_TTL_MS) x.v[0] = 0;
      if (!x.v[0] && !slot) slot = &x;
    }
    if (!slot) return fail(r, 429, "too many open nonces");
    uint8_t raw[16];
    esp_fill_random(raw, sizeof raw);
    b64urlEncode(raw, sizeof raw, slot->v, sizeof slot->v);
    slot->at = now;
    JsonDocument d;
    d["device_id"] = settings::cur.deviceId;
    d["nonce"] = slot->v;
    d["ttl_s"] = LOCAL_NONCE_TTL_MS / 1000;
    answer(r, 200, d);
  });
  server->on("/local/v1/command", HTTP_POST, handleCommand, nullptr, onBody);
  server->on("/local/v1/state", HTTP_POST, handleState, nullptr, onBody);
  server->onNotFound([](AsyncWebServerRequest* r) { fail(r, 404, "not found"); });
  server->begin();
  LOGI("local", "local control on port %u (%s)", listenPort, keySet() ? "key set" : "no key yet");
}

void startMdns() {
  if (!MDNS.begin(settings::cur.hostname)) {
    LOGW("local", "mDNS did not start");
    return;
  }
  MDNS.addService("titiswitch", "tcp", listenPort);
  MDNS.addServiceTxt("titiswitch", "tcp", "id", String(settings::cur.deviceId));
  mdnsOn = true;
}

bool privateIp(const IPAddress& ip) {
  return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 192 && ip[1] == 168);
}
}  // namespace

void begin() {
  logMtx = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin("loc", true)) {
    keyLen = p.getBytes("key", key, sizeof key);
    keyVersion = p.getUInt("ver", 0);
    listenPort = p.getUShort("port", LOCAL_DEFAULT_PORT);
    p.end();
  }
}

void loop() {
  if (!settings::cur.localEnabled || !settings::cur.deviceId[0]) return;
  if (!net::up()) return;
  if (!server) {
    startServer();
    startMdns();
  }
  if (keyChanged) {
    keyChanged = false;
    for (Nonce& x : nonces) x.v[0] = 0;        // nonces of the old key end with it
  }
}

// پیام config از هاب (از MQTT یا HTTP): {"type":"config","local":{enabled,key,version,port}}
// یا از هاب مرکزی ساختمان: {"type":"time","server_time":"<ISO UTC>","utc_offset_min":210}.
// کلید تازه در NVS ذخیره می‌شود؛ nonceهای باز کلید قدیمی باطل می‌شوند. پورت جدید پس از ریست بعدی اعمال می‌شود.
void applyConfig(const char* json, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return;
  if (!strcmp(doc["type"] | "", "time")) {     // هاب مرکزی: ساعت بدون اینترنت (و هر ۶ ساعت)
    rules::setClock(doc["server_time"] | "", true);
    if (doc["utc_offset_min"].is<int>()) rules::setOffset(doc["utc_offset_min"].as<int>());
    return;
  }
  JsonVariantConst l = doc["local"];
  if (!l.is<JsonObjectConst>()) return;
  bool enabled = l["enabled"] | false;
  uint8_t k[32];
  size_t kl = enabled ? b64urlDecode(l["key"] | "", k, sizeof k) : 0;
  uint32_t ver = l["version"] | 0;
  uint16_t prt = l["port"] | LOCAL_DEFAULT_PORT;
  portENTER_CRITICAL(&mux);
  bool same = kl == keyLen && !memcmp(k, key, kl) && ver == keyVersion;
  if (!same) {
    memcpy(key, k, kl);
    keyLen = kl;
    keyVersion = ver;
  }
  portEXIT_CRITICAL(&mux);
  if (same) return;
  keyChanged = true;
  Preferences p;
  if (p.begin("loc", false)) {
    if (kl) p.putBytes("key", k, kl); else p.remove("key");
    p.putUInt("ver", ver);
    p.putUShort("port", prt);
    p.end();
  }
  if (prt != listenPort) LOGI("local", "local port %u used after the next restart", prt);
  LOGI("local", "local key %s (version %lu)", kl ? "received" : "removed", (unsigned long)ver);
}

bool keySet() { return keyLen == 32; }
uint16_t port() { return listenPort; }

String addr() {
  if (!net::up()) return String();
  IPAddress ip;
  ip.fromString(net::ip());                       // وای‌فای یا LAN، هر کدام فعال است
  return privateIp(ip) ? ip.toString() : String();
}

// ثبت یک تغییر بدون سرور (فرمان محلی یا قاعدهٔ آفلاین). اگر به سرور وصل نیستیم تا اتصال بعدی نگه
// داشته می‌شود؛ اگر پر شود قدیمی‌ترین حذف می‌شود.
void log(uint8_t relay, bool value, const char* by) {
  if (!logMtx || xSemaphoreTake(logMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (logCount == LOCAL_LOG_ENTRIES) {                      // keep the newest
    memmove(logRing, logRing + 1, sizeof(LogEntry) * (LOCAL_LOG_ENTRIES - 1));
    logCount--;
  }
  LogEntry& e = logRing[logCount++];
  e.atMs = net::epochMs();
  e.relay = relay;
  e.value = value;
  strlcpy(e.by, by, sizeof e.by);
  xSemaphoreGive(logMtx);
  if (cloud::connected()) flushLog();
}

void flushLog() {
  if (!logCount || !cloud::connected() || !logMtx) return;
  if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
  JsonDocument d;
  JsonArray a = d["entries"].to<JsonArray>();
  char tag[10], at[32];
  for (uint8_t i = 0; i < logCount; i++) {
    JsonObject o = a.add<JsonObject>();
    if (logRing[i].atMs) {
      time_t t = logRing[i].atMs / 1000;
      struct tm tm;
      gmtime_r(&t, &tm);
      strftime(at, sizeof at, "%Y-%m-%dT%H:%M:%SZ", &tm);
      o["at"] = at;
    }
    snprintf(tag, sizeof tag, "relay_%u", logRing[i].relay + 1);
    o["tag"] = tag;
    o["value"] = logRing[i].value;
    o["by"] = logRing[i].by;
  }
  String body;
  serializeJson(d, body);
  bool sent = cloud::publishKind("local_log", body);
  if (sent) logCount = 0;
  xSemaphoreGive(logMtx);
}
}  // namespace local
