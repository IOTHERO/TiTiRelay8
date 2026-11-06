#include "portal.h"
#include <AsyncJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include "health.h"
#include "inputs.h"
#include "leds.h"
#include "cloud.h"
#include "log.h"
#include "net.h"
#include "relays.h"
#include "settings.h"
#include "web_ui.h"

namespace portal {
namespace {
constexpr uint8_t MAX_TOKENS = 4;
constexpr uint32_t TOKEN_TTL_MS = 8UL * 3600 * 1000;
constexpr uint8_t MAX_FAILS = 5;
constexpr uint32_t LOCKOUT_MS = 60000;
constexpr uint32_t PUSH_MS = 400;
constexpr uint8_t MAX_WS = 4;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
IPAddress apIp(PORTAL_IP[0], PORTAL_IP[1], PORTAL_IP[2], PORTAL_IP[3]);

struct Token { char v[33]; uint32_t at; };
Token tokens[MAX_TOKENS] = {};
uint8_t fails = 0;
uint32_t lockedAt = 0;
uint32_t wsAuthed[MAX_WS] = {0};

volatile uint32_t restartAt = 0;
volatile bool startWifiTest = false;
volatile int8_t startMqttTest = -1;
volatile bool otaOk = false;
String otaError;
bool netStarted = false;
bool fsOk = false;
uint32_t lastPush = 0;
uint32_t lastLogSent = 0;

// Relay walk test: each fitted relay ON for 1 s in turn.
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
    if (k.v[0] && now - k.at < TOKEN_TTL_MS && !strcmp(k.v, t)) return true;
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

// Route helper: JSON body + login required.
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

void routes() {
  // The UI lives in LittleFS (data/index.html, or index.html.gz); without it a fallback page
  // that can install a LittleFS image.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
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
    if (fails >= MAX_FAILS && now - lockedAt < LOCKOUT_MS) return sendError(r, 429, "locked");
    if (fails >= MAX_FAILS) fails = 0;
    const char* pass = j["password"] | "";
    if (strcmp(pass, settings::cur.adminPass) != 0) {
      if (++fails >= MAX_FAILS) lockedAt = now;
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
    d["adminDefault"] = strcmp(settings::cur.adminPass, "admin") == 0;
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
    settings::toJson(settings::cur, d.to<JsonObject>(), false);
    d["caSet"] = settings::caPem().length() > 0;
    sendJson(r, 200, d);
  });

  jsonRoute("/api/settings", [](AsyncWebServerRequest* r, JsonObjectConst in) {
    Settings* s = new Settings(settings::cur);
    settings::fromJson(*s, in);
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
    settings::fromJson(*s, in);
    String err;
    bool ok = settings::validate(*s, err) && settings::save(*s);
    delete s;
    ok ? sendOk(r) : sendError(r, 400, "invalid", err.c_str());
  }, 8192);

  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return sendError(r, 401, "login");
    JsonDocument d;
    settings::toJson(settings::cur, d.to<JsonObject>(), true);
    d["model"] = FW_MODEL;
    d["fw"] = FW_VERSION;
    String s;
    serializeJsonPretty(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Content-Disposition", "attachment; filename=titi-relay8-backup.json");
    r->send(res);
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
        // ?fs=1: a LittleFS image (the web UI) instead of the firmware.
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
    // Captive portal: any page on any host opens the UI.
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
  WiFi.softAP(ssid.c_str(), settings::cur.apPass, 6, 0, 4);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIp);
  routes();
  server.begin();
  leds::run(leds::RUN_CONFIG);
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
  if (walkIdx >= 0 && now - walkAt >= 1000) {
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
  if (now - lastPush >= PUSH_MS) {
    lastPush = now;
    ws.cleanupClients(MAX_WS);
    pushState();
  }
  if (restartAt && (int32_t)(now - restartAt) >= 0) {
    ws.closeAll();
    health::restart("requested from the config portal");
  }
}
}  // namespace portal
