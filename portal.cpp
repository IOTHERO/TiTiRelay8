// =====================================================================================
//  portal.cpp — منوی تنظیمات (وب)
// =====================================================================================
//
//  وقتی کلید کانفیگ (دیپ ۸) هنگام روشن شدن روشن باشد (یا هنوز هیچ ارتباطی انتخاب نشده)، برد
//  یک شبکهٔ وای‌فای «TiTi-R8-xxxx» می‌سازد (نشانی 192.168.1.100، رمز در config.h) و منو را نشان می‌دهد.
//
//  ● ظاهر منو (data/index.html و data/static/qr.js) روی حافظهٔ LittleFS است و از آنجا خوانده می‌شود.
//    اگر هنوز بارگذاری نشده باشد، یک صفحهٔ ساده (FALLBACK_HTML) نمایش داده می‌شود که اجازهٔ نصب
//    تصویر LittleFS را می‌دهد.
//  ● ورود با رمز مدیر؛ پس از چند بار رمز اشتباه، ورود مدتی قفل می‌شود. هر ورود یک «توکن» می‌دهد که
//    در هدر X-Auth فرستاده می‌شود.
//  ● وضعیت زنده (رله‌ها، ورودی‌ها، حافظه، گزارش) با WebSocket هر PORTAL_PUSH_MS به مرورگر می‌رسد.
//  ● تنظیمات به JSON تبدیل می‌شوند (toJson/fromJson): رمزها هرگز به مرورگر فرستاده نمی‌شوند
//    (فقط «ثبت شده است»)، و رمز خالی در ذخیره یعنی «بدون تغییر».
//  ● هر صفحهٔ نشانی ناشناخته به منو هدایت می‌شود (Captive portal)، پس گوشی خودش منو را باز می‌کند.
//  ● در حالت تنظیمات، وای‌فای ایستگاه و MQTT فقط برای دکمه‌های «تست اتصال» روشن می‌شوند، نه برای کار عادی.
// =====================================================================================
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include "core.h"

namespace portal {
namespace {

// صفحهٔ ساده‌ای که وقتی LittleFS (رابط وب) بارگذاری نشده نمایش داده می‌شود:
static const char FALLBACK_HTML[] = R"HTML(<!doctype html>
<html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TiTi Relay-8</title><style>body{font-family:Tahoma,system-ui,sans-serif;background:#0a0c11;color:#eef0f6;display:grid;place-items:center;min-height:100vh;margin:0;padding:20px}
.b{max-width:440px;padding:28px;border:1px solid rgba(255,255,255,.1);border-radius:20px;background:rgba(255,255,255,.04)}h1{color:#d9b46a;font-size:20px}
p{color:#9aa3b5;line-height:1.8}code{direction:ltr;display:inline-block;background:#000;padding:2px 6px;border-radius:6px}
input,button{font:inherit;margin-top:10px;width:100%;padding:10px;border-radius:10px;border:1px solid #333;background:#111;color:#eee}button{background:#d9b46a;color:#1a1307;border:0;font-weight:700}</style></head>
<body><div class="b"><h1>TiTi Relay-8</h1>
<p>فایل‌های رابط کاربری (LittleFS) روی حافظه نیست. پوشهٔ <code>data</code> را با «Upload LittleFS» در Arduino IDE بارگذاری کنید، یا فایل <code>littlefs.bin</code> را این‌جا نصب کنید.</p>
<p dir="ltr" style="text-align:left">The web UI (LittleFS) is missing. Upload the <code>data</code> folder with "Upload LittleFS", or install a LittleFS image below.</p>
<input id="p" type="password" placeholder="admin password"><input id="f" type="file" accept=".bin"><button id="u">Upload filesystem image</button><p id="s"></p></div>
<script>
document.getElementById("u").onclick=async()=>{const s=document.getElementById("s");
const r=await fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({password:document.getElementById("p").value})});
if(!r.ok){s.textContent="password?";return}const tk=(await r.json()).token;const f=document.getElementById("f").files[0];if(!f)return;
const fd=new FormData();fd.append("fs",f,f.name);s.textContent="...";
const x=await fetch("/api/ota?fs=1",{method:"POST",headers:{"X-Auth":tk},body:fd});s.textContent=x.ok?"OK, restarting":"failed";if(x.ok)setTimeout(()=>location.reload(),9000)};
</script></body></html>
)HTML";

// ── تبدیل Settings به JSON و برعکس (برای منو) ────────────────────────────────────────
// مقدارهایی که در JSON نباشند، در Settings بدون تغییر می‌مانند.

void str(JsonObjectConst in, const char* key, char* dst, size_t cap) {
  if (in[key].is<const char*>()) strlcpy(dst, in[key].as<const char*>(), cap);
}

// رمزها: مقدار خالی یا غایب = «بدون تغییر» (منو هرگز رمز ذخیره‌شده را نمی‌گیرد).
void secret(JsonObjectConst in, const char* key, char* dst, size_t cap) {
  const char* v = in[key] | "";
  if (*v) strlcpy(dst, v, cap);
}

template <typename T> void num(JsonObjectConst in, const char* key, T& dst) {
  if (in[key].is<long>()) dst = (T)in[key].as<long>();
  else if (in[key].is<bool>()) dst = (T)in[key].as<bool>();
}

void ipFrom(JsonObjectConst in, const char* key, uint32_t& dst) {
  IPAddress ip;
  if (in[key].is<const char*>() && ip.fromString(in[key].as<const char*>())) dst = (uint32_t)ip;
}

String ipStr(uint32_t v) { return IPAddress(v).toString(); }

// withSecrets=true فقط برای فایل پشتیبان (دانلود با ورود مدیر) است.
void toJson(const Settings& s, JsonObject o, bool withSecrets) {
  o["deviceName"] = s.deviceName;
  o["mode"] = s.mode;
  JsonObject w = o["wifi"].to<JsonObject>();
  JsonArray nets = w["nets"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_NETS; i++) {
    JsonObject n = nets.add<JsonObject>();
    n["ssid"] = s.nets[i].ssid;
    n["passSet"] = s.nets[i].pass[0] != 0;
    if (withSecrets) n["pass"] = s.nets[i].pass;
  }
  w["dhcp"] = s.dhcp;
  w["ip"] = ipStr(s.ip);
  w["gateway"] = ipStr(s.gateway);
  w["subnet"] = ipStr(s.subnet);
  w["dns1"] = ipStr(s.dns1);
  w["dns2"] = ipStr(s.dns2);
  w["hostname"] = s.hostname;
  w["ntp"] = s.ntp;
  JsonArray sv = o["servers"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_SERVERS; i++) {
    const ServerCfg& v = s.servers[i];
    JsonObject j = sv.add<JsonObject>();
    j["enabled"] = v.enabled;
    j["proto"] = v.proto;
    j["name"] = v.name;
    j["host"] = v.host;
    j["port"] = v.port;
    j["tls"] = v.tls;
    j["user"] = v.user;
    j["passSet"] = v.pass[0] != 0;
    if (withSecrets) j["pass"] = v.pass;
    j["clientId"] = v.clientId;
    j["prefix"] = v.prefix;
    j["keepalive"] = v.keepalive;
    j["qos"] = v.qos;
  }
  o["deviceId"] = s.deviceId;
  o["telemetryS"] = s.telemetryS;
  o["apiUrl"] = s.apiUrl;
  o["localEnabled"] = s.localEnabled;
  o["otaEnabled"] = s.otaEnabled;
  JsonObject m = o["mb"].to<JsonObject>();
  m["fromDip"] = s.mbAddrFromDip;
  m["addr"] = s.mbAddr;
  m["baud"] = s.mbBaud;
  m["parity"] = s.mbParity;
  m["stopBits"] = s.mbStopBits;
  m["respDelayMs"] = s.mbRespDelayMs;
  m["swap"] = s.mbSwapRxTx;
  m["wordSwap"] = s.mbWordSwap;
  JsonArray base = m["base"].to<JsonArray>();           // آدرس شروع جدول‌ها: Coil، Discrete، Input، Holding
  for (uint8_t t = 0; t < 4; t++) base.add(s.mbBase[t]);
  m["lenient"] = s.mbLenient;
  JsonObject l = o["link"].to<JsonObject>();
  l["timeoutS"] = s.linkTimeoutS;
  l["action"] = s.linkLoss;
  l["mask"] = s.linkLossMask;
  JsonObject r = o["relays"].to<JsonObject>();
  r["minIntervalMs"] = s.minIntervalMs;
  r["staggerMs"] = s.staggerMs;
  JsonArray items = r["items"].to<JsonArray>();
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    JsonObject it = items.add<JsonObject>();
    it["name"] = s.relayName[i];
    it["powerOn"] = s.powerOn[i];
    it["interlock"] = s.interlock[i];
    it["fitted"] = RELAY_PINS[i] >= 0;
  }
  JsonObject sy = o["system"].to<JsonObject>();
  sy["logLevel"] = s.logLevel;
  sy["apPassSet"] = true;
  sy["adminDefault"] = strcmp(s.adminPass, DEFAULT_ADMIN_PASSWORD) == 0;   // منو هشدار می‌دهد رمز پیش‌فرض را عوض کنید
  if (withSecrets) {
    sy["apPass"] = s.apPass;
    sy["adminPass"] = s.adminPass;
  }
}

void fromJson(Settings& s, JsonObjectConst o) {
  str(o, "deviceName", s.deviceName, sizeof s.deviceName);
  num(o, "mode", s.mode);
  JsonObjectConst w = o["wifi"];
  if (w) {
    JsonArrayConst nets = w["nets"];
    for (uint8_t i = 0; i < MAX_NETS && i < nets.size(); i++) {
      JsonObjectConst n = nets[i];
      if (n["ssid"].is<const char*>()) {
        const char* ssid = n["ssid"];
        // شبکه‌ای که عوض یا پاک شود، رمز قبلی‌اش را هم فراموش می‌کند
        if (strcmp(ssid, s.nets[i].ssid) != 0) s.nets[i].pass[0] = 0;
        strlcpy(s.nets[i].ssid, ssid, sizeof s.nets[i].ssid);
      }
      secret(n, "pass", s.nets[i].pass, sizeof s.nets[i].pass);
      if (n["clearPass"] | false) s.nets[i].pass[0] = 0;
    }
    num(w, "dhcp", s.dhcp);
    ipFrom(w, "ip", s.ip);
    ipFrom(w, "gateway", s.gateway);
    ipFrom(w, "subnet", s.subnet);
    ipFrom(w, "dns1", s.dns1);
    ipFrom(w, "dns2", s.dns2);
    str(w, "hostname", s.hostname, sizeof s.hostname);
    str(w, "ntp", s.ntp, sizeof s.ntp);
  }
  JsonArrayConst sv = o["servers"];
  for (uint8_t i = 0; i < MAX_SERVERS && i < sv.size(); i++) {
    JsonObjectConst j = sv[i];
    ServerCfg& v = s.servers[i];
    num(j, "enabled", v.enabled);
    num(j, "proto", v.proto);
    str(j, "name", v.name, sizeof v.name);
    if (j["host"].is<const char*>() && strcmp(j["host"], v.host) != 0) v.pass[0] = 0;   // سرور عوض شد: رمز قبلی را نگه نمی‌داریم
    str(j, "host", v.host, sizeof v.host);
    num(j, "port", v.port);
    num(j, "tls", v.tls);
    str(j, "user", v.user, sizeof v.user);
    secret(j, "pass", v.pass, sizeof v.pass);
    if (j["clearPass"] | false) v.pass[0] = 0;
    str(j, "clientId", v.clientId, sizeof v.clientId);
    str(j, "prefix", v.prefix, sizeof v.prefix);
    num(j, "keepalive", v.keepalive);
    num(j, "qos", v.qos);
  }
  str(o, "deviceId", s.deviceId, sizeof s.deviceId);
  num(o, "telemetryS", s.telemetryS);
  str(o, "apiUrl", s.apiUrl, sizeof s.apiUrl);
  num(o, "localEnabled", s.localEnabled);
  num(o, "otaEnabled", s.otaEnabled);
  JsonObjectConst m = o["mb"];
  if (m) {
    num(m, "fromDip", s.mbAddrFromDip);
    num(m, "addr", s.mbAddr);
    num(m, "baud", s.mbBaud);
    num(m, "parity", s.mbParity);
    num(m, "stopBits", s.mbStopBits);
    num(m, "respDelayMs", s.mbRespDelayMs);
    num(m, "swap", s.mbSwapRxTx);
    num(m, "wordSwap", s.mbWordSwap);
    num(m, "lenient", s.mbLenient);
    JsonArrayConst bs = m["base"];
    for (uint8_t t = 0; t < 4 && t < bs.size(); t++) {
      if (!bs[t].is<long>()) continue;
      long v = bs[t].as<long>();
      s.mbBase[t] = v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v;   // مقدار بزرگ‌تر از ۱۶ بیت را validate رد می‌کند
    }
  }
  JsonObjectConst l = o["link"];
  if (l) {
    num(l, "timeoutS", s.linkTimeoutS);
    num(l, "action", s.linkLoss);
    num(l, "mask", s.linkLossMask);
  }
  JsonObjectConst r = o["relays"];
  if (r) {
    num(r, "minIntervalMs", s.minIntervalMs);
    num(r, "staggerMs", s.staggerMs);
    JsonArrayConst items = r["items"];
    for (uint8_t i = 0; i < RELAY_COUNT && i < items.size(); i++) {
      JsonObjectConst it = items[i];
      str(it, "name", s.relayName[i], sizeof s.relayName[i]);
      num(it, "powerOn", s.powerOn[i]);
      num(it, "interlock", s.interlock[i]);
    }
  }
  JsonObjectConst sy = o["system"];
  if (sy) {
    num(sy, "logLevel", s.logLevel);
    secret(sy, "apPass", s.apPass, sizeof s.apPass);
    secret(sy, "adminPass", s.adminPass, sizeof s.adminPass);
  }
}

// ── وضعیت منو ────────────────────────────────────────────────────────────────────────────

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
IPAddress apIp(PORTAL_IP[0], PORTAL_IP[1], PORTAL_IP[2], PORTAL_IP[3]);

// توکن ورود: رشتهٔ تصادفی ۳۲ حرفی؛ تا PORTAL_MAX_TOKENS نشست هم‌زمان، قدیمی‌ترین جایگزین می‌شود.
struct Token { char v[33]; uint32_t at; };
Token tokens[PORTAL_MAX_TOKENS] = {};
uint8_t fails = 0;
uint32_t lockedAt = 0;
uint32_t wsAuthed[PORTAL_MAX_WS] = {0};

volatile uint32_t restartAt = 0;
volatile bool startWifiTest = false;
volatile int8_t startMqttTest = -1;
volatile bool otaOk = false;
String otaError;
bool netStarted = false;
bool fsOk = false;
uint32_t lastPush = 0;
uint32_t lastLogSent = 0;

// «تست ترتیبی» رله‌ها: هر رلهٔ نصب‌شده به نوبت PORTAL_WALK_STEP_MS روشن می‌ماند.
volatile bool walkStart = false;
int8_t walkIdx = -1;
uint32_t walkAt = 0;

void newToken(char* out) {
  for (uint8_t i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", (uint8_t)esp_random());
  out[32] = 0;
}

bool tokenOk(const char* t) {
  if (!t || strlen(t) != 32) return false;
  uint32_t now = millis();
  for (Token& k : tokens)
    if (k.v[0] && now - k.at < PORTAL_TOKEN_TTL_MS && !strcmp(k.v, t)) return true;
  return false;
}

bool authed(AsyncWebServerRequest* r) {
  const AsyncWebHeader* h = r->getHeader("X-Auth");
  return h && tokenOk(h->value().c_str());
}

void sendJson(AsyncWebServerRequest* r, int code, JsonDocument& d) {
  String s;
  serializeJson(d, s);
  r->send(code, "application/json", s);
}

void sendError(AsyncWebServerRequest* r, int code, const char* error, const char* field = nullptr) {
  JsonDocument d;
  d["error"] = error;
  if (field) d["field"] = field;
  sendJson(r, code, d);
}

void sendOk(AsyncWebServerRequest* r) { r->send(200, "application/json", "{\"ok\":true}"); }

// مسیر POST با بدنهٔ JSON که ورود مدیر می‌خواهد (بدون توکن معتبر: خطای 401).
void jsonRoute(const char* uri, std::function<void(AsyncWebServerRequest*, JsonObjectConst)> fn,
               size_t maxLen = 4096) {
  auto* h = new AsyncCallbackJsonWebHandler(uri, [fn](AsyncWebServerRequest* r, JsonVariant& j) {
    if (!authed(r)) return sendError(r, 401, "login");
    fn(r, j.as<JsonObjectConst>());
  });
  h->setMethod(HTTP_POST);
  h->setMaxContentLength(maxLen);
  server.addHandler(h);
}

void buildState(JsonDocument& d, bool withLog) {
  d["t"] = "state";
  d["relays"] = relays::state();
  d["pulsing"] = relays::pulsing();
  d["fitted"] = relays::fittedMask();
  JsonArray cyc = d["cycles"].to<JsonArray>();
  for (uint8_t r = 0; r < RELAY_COUNT; r++) cyc.add(relays::cycles(r));
  d["dip"] = inputs::raw();
  d["addr"] = settings::modbusAddress();
  d["cfg"] = inputs::configOn();
  d["up"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["minHeap"] = ESP.getMinFreeHeap();
  d["safe"] = health::safeMode();
  d["reset"] = health::resetReason();
  d["boots"] = health::bootCount();
  d["walk"] = walkIdx;
  JsonObject w = d["wifi"].to<JsonObject>();
  w["st"] = netStarted ? net::status() : "idle";
  w["ip"] = net::ip();
  w["ssid"] = net::ssid();
  w["rssi"] = net::rssi();
  d["mqttTest"] = cloud::testResult();
  d["clients"] = WiFi.softAPgetStationNum();
  d["localKey"] = local::keySet();
  d["rules"] = rules::count();
  d["rulesVer"] = rules::version();
  if (withLog) {
    logx::Entry e[12];
    uint8_t n = logx::since(lastLogSent, e, 12);
    JsonArray l = d["log"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) {
      JsonArray x = l.add<JsonArray>();
      x.add(e[i].ms / 1000);
      x.add(e[i].lvl);
      x.add(e[i].text);
      lastLogSent = e[i].seq;
    }
  }
}

void onWs(AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data,
          size_t len) {
  if (type == WS_EVT_DISCONNECT) {
    for (uint32_t& id : wsAuthed) if (id == c->id()) id = 0;
    return;
  }
  if (type != WS_EVT_DATA) return;
  auto* info = static_cast<AwsFrameInfo*>(arg);
  if (!info->final || info->index != 0 || info->len != len || len > 200) return;
  JsonDocument d;
  if (deserializeJson(d, data, len)) return;
  if (!tokenOk(d["auth"] | "")) { c->close(); return; }
  for (uint32_t& id : wsAuthed) {
    if (id == 0) { id = c->id(); lastLogSent = 0; return; }   // new viewer gets the log ring
  }
  c->close();
}

void pushState() {
  bool any = false;
  for (uint32_t id : wsAuthed) any |= id != 0;
  if (!any) return;
  JsonDocument d;
  buildState(d, true);
  String s;
  serializeJson(d, s);
  for (uint32_t id : wsAuthed)
    if (id && ws.availableForWrite(id)) ws.text(id, s);
}

// عیب‌یابی اتصال: ورود و خروج گوشی یا کامپیوتر به وای‌فای خود برد، و IP که از برد گرفته،
// در گزارش سریال نوشته می‌شود. اگر «client joined» بیاید ولی «client got IP» نه، مشکل DHCP است؛
// اگر IP گرفته ولی «page requested» نیامد، درخواست به برد نرسیده (مسیر اشتباه در گوشی یا کامپیوتر).
void onApEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t* m = info.wifi_ap_staconnected.mac;
      LOGI("portal", "client joined the Wi-Fi: %02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      String ip = IPAddress(info.wifi_ap_staipassigned.ip.addr).toString();
      LOGI("portal", "client got IP %s", ip.c_str());
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      LOGI("portal", "client left the Wi-Fi");
      break;
    default:
      break;
  }
}

void routes() {
  // ظاهر منو در LittleFS است (data/index.html)؛ در غیر این صورت صفحهٔ ساده‌ای که امکان نصب تصویر
  // LittleFS را می‌دهد.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    String from = r->client()->remoteIP().toString();
    LOGI("portal", "page requested by %s", from.c_str());
    if (fsOk && (LittleFS.exists("/index.html") || LittleFS.exists("/index.html.gz"))) {
      AsyncWebServerResponse* res = r->beginResponse(LittleFS, "/index.html", "text/html");
      res->addHeader("Cache-Control", "no-store");
      return r->send(res);
    }
    r->send(200, "text/html", (const uint8_t*)FALLBACK_HTML, strlen(FALLBACK_HTML));
  });
  if (fsOk) server.serveStatic("/static/", LittleFS, "/static/").setCacheControl("max-age=86400");

  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    d["fw"] = FW_VERSION;
    d["model"] = FW_MODEL;
    d["name"] = settings::cur.deviceName;
    d["mac"] = WiFi.macAddress();
    sendJson(r, 200, d);
  });

  auto* login = new AsyncCallbackJsonWebHandler("/api/login", [](AsyncWebServerRequest* r, JsonVariant& j) {
    uint32_t now = millis();
    if (fails >= PORTAL_MAX_FAILS && now - lockedAt < PORTAL_LOCKOUT_MS) return sendError(r, 429, "locked");
    if (fails >= PORTAL_MAX_FAILS) fails = 0;
    const char* pass = j["password"] | "";
    if (strcmp(pass, settings::cur.adminPass) != 0) {
      if (++fails >= PORTAL_MAX_FAILS) lockedAt = now;
      LOGW("portal", "wrong password (%u)", fails);
      return sendError(r, 403, "password");
    }
    fails = 0;
    Token* slot = &tokens[0];
    for (Token& k : tokens) if (k.at < slot->at || !k.v[0]) { slot = &k; if (!k.v[0]) break; }
    newToken(slot->v);
    slot->at = now;
    LOGI("portal", "signed in");
    JsonDocument d;
    d["token"] = slot->v;
    d["adminDefault"] = strcmp(settings::cur.adminPass, DEFAULT_ADMIN_PASSWORD) == 0;
    sendJson(r, 200, d);
  });
  login->setMethod(HTTP_POST);
  login->setMaxContentLength(256);
  server.addHandler(login);

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    JsonDocument d;
    buildState(d, false);
    sendJson(r, 200, d);
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    JsonDocument d;
    toJson(settings::cur, d.to<JsonObject>(), false);
    d["caSet"] = settings::caPem().length() > 0;
    sendJson(r, 200, d);
  });

  jsonRoute("/api/settings", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    Settings* s = new Settings(settings::cur);
    fromJson(*s, in);
    String err;
    if (!settings::validate(*s, err)) {
      delete s;
      return sendError(r, 400, "invalid", err.c_str());
    }
    bool ok = settings::save(*s);
    delete s;
    logx::setLevel(settings::cur.logLevel);
    ok ? sendOk(r) : sendError(r, 500, "storage");
  }, 8192);

  jsonRoute("/api/restore", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    Settings* s = new Settings;
    settings::defaults(*s);
    fromJson(*s, in);
    String err;
    bool ok = settings::validate(*s, err) && settings::save(*s);
    delete s;
    ok ? sendOk(r) : sendError(r, 400, "invalid", err.c_str());
  }, 8192);

  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    JsonDocument d;
    toJson(settings::cur, d.to<JsonObject>(), true);
    d["model"] = FW_MODEL;
    d["fw"] = FW_VERSION;
    String s;
    serializeJsonPretty(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Content-Disposition", "attachment; filename=titi-relay8-backup.json");
    r->send(res);
  });

  server.on("/api/label", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    settings::Label l = settings::label();
    JsonDocument d;
    d["qr"] = l.qr;
    d["serial"] = l.serial;
    sendJson(r, 200, d);
  });

  jsonRoute("/api/label", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    settings::setLabel(in["qr"] | "", in["serial"] | "") ? sendOk(r) : sendError(r, 400, "invalid", "qr");
  });

  jsonRoute("/api/ca", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    settings::setCaPem(in["pem"] | "") ? sendOk(r) : sendError(r, 400, "invalid", "pem");
  }, 6144);

  jsonRoute("/api/relay", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    bool ok;
    if (in["all"].is<bool>()) {
      ok = in["all"].as<bool>() ? relays::setMask(0xFF, relays::SRC_WEB) : relays::allOff(relays::SRC_WEB, false);
    } else if (in["mask"].is<int>()) {
      ok = relays::setMask(in["mask"].as<int>(), relays::SRC_WEB);
    } else {
      int i = in["r"] | -1;
      if (i < 0 || i >= RELAY_COUNT) return sendError(r, 400, "invalid", "r");
      if (!relays::fitted(i)) return sendError(r, 409, "not_fitted");
      uint32_t pulse = in["pulseMs"] | 0;
      if (pulse > 600000) return sendError(r, 400, "invalid", "pulseMs");
      ok = relays::set(i, pulse ? true : (bool)(in["on"] | false), relays::SRC_WEB, pulse);
    }
    walkIdx = -1;
    ok ? sendOk(r) : sendError(r, 503, "busy");
  });

  jsonRoute("/api/action", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    const char* a = in["do"] | "";
    if (!strcmp(a, "identify")) leds::identify(10000);
    else if (!strcmp(a, "walk")) walkStart = true;
    else if (!strcmp(a, "walkStop")) { walkIdx = -1; relays::allOff(relays::SRC_WEB, false); }
    else if (!strcmp(a, "wifiTest")) startWifiTest = true;
    else if (!strcmp(a, "mqttTest")) startMqttTest = in["server"] | 0;
    else if (!strcmp(a, "restart")) restartAt = millis() + 800;
    else if (!strcmp(a, "factory")) {
      if (strcmp(in["confirm"] | "", "RESET") != 0) return sendError(r, 400, "confirm");
      settings::factoryReset();
      restartAt = millis() + 800;
    } else return sendError(r, 400, "unknown");
    sendOk(r);
  });

  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    int n = WiFi.scanComplete();
    JsonDocument d;
    if (n == WIFI_SCAN_RUNNING) {
      d["running"] = true;
    } else if (n >= 0 && !r->hasParam("fresh")) {
      JsonArray a = d["nets"].to<JsonArray>();
      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        bool dup = false;
        for (JsonObject o : a) if (ssid == (const char*)o["ssid"]) { dup = true; break; }
        if (dup) continue;
        JsonObject o = a.add<JsonObject>();
        o["ssid"] = ssid;
        o["rssi"] = WiFi.RSSI(i);
        o["ch"] = WiFi.channel(i);
        o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
    } else {
      WiFi.scanDelete();
      WiFi.scanNetworks(true, false);
      d["running"] = true;
    }
    sendJson(r, 200, d);
  });

  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!authed(r)) return sendError(r, 401, "login");
      if (!otaOk) return sendError(r, 400, "ota", otaError.c_str());
      sendOk(r);
      restartAt = millis() + 1500;
    },
    [](AsyncWebServerRequest* r, const String& name, size_t index, uint8_t* data, size_t len, bool final) {
      if (!authed(r)) return;
      if (index == 0) {
        otaOk = false;
        otaError = "";
        // با ?fs=1 فایل ارسالی «تصویر LittleFS» (ظاهر منو) است، نه Firmware.
        bool fs = r->hasParam("fs");
        LOGI("ota", "%s upload: %s", fs ? "filesystem" : "firmware", name.c_str());
        if (fs && fsOk) { LittleFS.end(); fsOk = false; }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, fs ? U_SPIFFS : U_FLASH)) { otaError = Update.errorString(); return; }
      }
      if (otaError.length()) return;
      if (Update.write(data, len) != len) { otaError = Update.errorString(); Update.abort(); return; }
      if (final) {
        if (Update.end(true)) { otaOk = true; LOGI("ota", "image accepted (%u bytes)", (unsigned)(index + len)); }
        else { otaError = Update.errorString(); LOGE("ota", "image rejected: %s", otaError.c_str()); }
      }
    });

  server.onNotFound([](AsyncWebServerRequest* r) {
    // Captive portal: هر نشانی ناشناخته به منو هدایت می‌شود تا گوشی خودش منو را باز کند.
    if (r->url().startsWith("/api/")) return sendError(r, 404, "not_found");
    r->redirect("http://" + apIp.toString() + "/");
  });

  ws.onEvent(onWs);
  server.addHandler(&ws);
}
}  // namespace

void begin() {
  fsOk = LittleFS.begin(false);
  if (!fsOk) LOGW("portal", "LittleFS not found: upload the data folder (web UI); fallback page served");
  else if (!LittleFS.exists("/index.html") && !LittleFS.exists("/index.html.gz"))
    LOGW("portal", "LittleFS has no index.html: upload the data folder");
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  String ssid = settings::apSsid();
  WiFi.softAP(ssid.c_str(), settings::cur.apPass, PORTAL_AP_CHANNEL, 0, PORTAL_AP_MAX_CLIENTS);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIp);
  WiFi.onEvent(onApEvent);
  routes();
  server.begin();
  LOGI("portal", "web server on %s:80 (clients get %u.%u.%u.%u..)", WiFi.softAPIP().toString().c_str(),
       PORTAL_IP[0], PORTAL_IP[1], PORTAL_IP[2], PORTAL_IP[3] + 1);
  leds::run(leds::RUN_STEADY);   // config mode: RUN fixed on
  LOGI("portal", "config mode: Wi-Fi \"%s\", open http://%s/", ssid.c_str(), apIp.toString().c_str());
}

void loop() {
  uint32_t now = millis();
  dns.processNextRequest();
  if (startWifiTest) {
    startWifiTest = false;
    netStarted = true;
    net::begin(true);
  }
  if (startMqttTest >= 0) {
    cloud::test(startMqttTest);
    startMqttTest = -1;
  }
  if (netStarted) {
    net::loop();
    cloud::loop();
  }
  if (walkStart) {
    walkStart = false;
    walkIdx = 0;
    walkAt = 0;
  }
  if (walkIdx >= 0 && now - walkAt >= PORTAL_WALK_STEP_MS) {
    while (walkIdx < RELAY_COUNT && !relays::fitted(walkIdx)) walkIdx++;
    if (walkIdx >= RELAY_COUNT) {
      walkIdx = -1;
      relays::allOff(relays::SRC_WEB, false);
    } else {
      relays::setMask(1 << walkIdx, relays::SRC_WEB);
      walkIdx++;
      walkAt = now;
    }
  }
  if (now - lastPush >= PORTAL_PUSH_MS) {
    lastPush = now;
    ws.cleanupClients(PORTAL_MAX_WS);
    pushState();
  }
  if (restartAt && (int32_t)(now - restartAt) >= 0) {
    ws.closeAll();
    health::restart("requested from the config portal");
  }
}
}  // namespace portal
