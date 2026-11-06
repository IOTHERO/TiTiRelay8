#include "settings.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <stddef.h>
#include "inputs.h"
#include "log.h"

namespace settings {
Settings cur;

namespace {
constexpr uint16_t VERSION = 3;   // bump when fields are appended
constexpr const char* NS = "cfg";
constexpr const char* IDENT = "ident";
// The oldest layout still read: version 2 ended with logLevel.
constexpr size_t SIZE_V2 = offsetof(Settings, mbWordSwap);

// The production identity (and the link to reach the platform) is also kept in its own keys,
// apart from the blob: if the blob is ever unreadable the unit still reaches the platform.
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

// withLink: also the mode and the main Wi-Fi (the blob was lost, not reset on purpose).
bool restoreIdentity(Settings& s, bool withLink) {
  Preferences p;
  if (!p.begin(IDENT, true)) return false;
  bool any = false;
  String id = p.getString("id", "");
  if (id.length()) { strlcpy(s.deviceId, id.c_str(), sizeof s.deviceId); any = true; }
  strlcpy(s.apiUrl, p.getString("api", "").c_str(), sizeof s.apiUrl);
  ServerCfg v;
  if (p.getBytesLength("srv") == sizeof v && p.getBytes("srv", &v, sizeof v) == sizeof v) {
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
const uint32_t BAUDS[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};

String macSuffix() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);   // valid before Wi-Fi is started
  char b[5];
  snprintf(b, sizeof b, "%02X%02X", mac[4], mac[5]);
  return String(b);
}

void copyStr(char* dst, size_t cap, const char* src) {
  if (src == nullptr) return;
  strlcpy(dst, src, cap);
}

void str(JsonObjectConst in, const char* key, char* dst, size_t cap) {
  if (in[key].is<const char*>()) copyStr(dst, cap, in[key].as<const char*>());
}

// Passwords: an empty or missing value keeps the stored one (the portal never receives them).
void secret(JsonObjectConst in, const char* key, char* dst, size_t cap) {
  const char* v = in[key] | "";
  if (*v) copyStr(dst, cap, v);
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

bool printable(const char* s) {
  for (; *s; s++) if ((uint8_t)*s < 0x20) return false;
  return true;
}

bool topicSafe(const char* s) {
  for (; *s; s++) if (*s == '+' || *s == '#' || (uint8_t)*s < 0x20) return false;
  return true;
}
}  // namespace

void defaults(Settings& s) {
  memset(&s, 0, sizeof s);
  s.version = VERSION;
  copyStr(s.deviceName, sizeof s.deviceName, FW_NAME);
  s.mode = MODE_UNSET;
  s.dhcp = true;
  s.subnet = (uint32_t)IPAddress(255, 255, 255, 0);
  snprintf(s.hostname, sizeof s.hostname, "titi-r8-%s", macSuffix().c_str());
  for (char* p = s.hostname; *p; p++) *p = tolower(*p);
  copyStr(s.ntp, sizeof s.ntp, "pool.ntp.org");
  for (uint8_t i = 0; i < MAX_SERVERS; i++) {
    ServerCfg& v = s.servers[i];
    v.enabled = false;
    v.proto = PROTO_PLATFORM;
    snprintf(v.name, sizeof v.name, "Server %u", i + 1);
    v.port = 1883;
    v.keepalive = 30;
    v.qos = 1;
  }
  s.telemetryS = 30;
  s.localEnabled = true;
  s.otaEnabled = true;
  s.mbAddrFromDip = 1;
  s.mbAddr = 1;
  s.mbBaud = 9600;
  s.mbParity = 0;
  s.mbStopBits = 1;
  s.mbRespDelayMs = 0;
  s.mbSwapRxTx = false;
  s.linkTimeoutS = 0;
  s.linkLoss = LOSS_HOLD;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    s.powerOn[i] = PON_RESTORE;
    snprintf(s.relayName[i], sizeof s.relayName[i], "Relay %u", i + 1);
  }
  s.minIntervalMs = 100;
  s.staggerMs = 30;
  copyStr(s.apPass, sizeof s.apPass, "titi1234");
  copyStr(s.adminPass, sizeof s.adminPass, "admin");
  s.logLevel = LOG_INFO;
}

bool validate(Settings& s, String& err) {
  // Make every string terminated even if the blob was damaged.
  s.deviceName[sizeof s.deviceName - 1] = 0;
  s.hostname[sizeof s.hostname - 1] = 0;
  s.deviceId[sizeof s.deviceId - 1] = 0;
  if (s.mode > MODE_RS485) { err = "mode"; return false; }
  if (!printable(s.deviceName)) { err = "deviceName"; return false; }
  if (s.mode == MODE_WIFI) {
    if (!s.nets[0].ssid[0]) { err = "wifi.ssid"; return false; }
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
  if (au && s.apiUrl[au - 1] == '/') s.apiUrl[au - 1] = 0;
  if (s.telemetryS < 5 || s.telemetryS > 3600) { err = "telemetryS"; return false; }
  bool baudOk = false;
  for (uint32_t b : BAUDS) baudOk |= b == s.mbBaud;
  if (!baudOk) { err = "mb.baud"; return false; }
  if (s.mbAddr < 1 || s.mbAddr > 247) { err = "mb.addr"; return false; }
  if (s.mbParity > 2 || s.mbStopBits < 1 || s.mbStopBits > 2) { err = "mb.format"; return false; }
  if (s.mbRespDelayMs > 500) { err = "mb.respDelay"; return false; }
  if (s.linkTimeoutS > 3600 || s.linkLoss > LOSS_PRESET) { err = "linkLoss"; return false; }
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    s.relayName[i][sizeof s.relayName[i] - 1] = 0;
    if (s.powerOn[i] > PON_ON) { err = "relays.powerOn"; return false; }
    uint8_t p = s.interlock[i];
    if (p > RELAY_COUNT || p == i + 1) { err = "relays.interlock"; return false; }
    // Interlocks are pairs: the partner must point back.
    if (p && s.interlock[p - 1] != i + 1) { err = "relays.interlock"; return false; }
  }
  if (s.minIntervalMs > 10000 || s.staggerMs > 1000) { err = "relays.timing"; return false; }
  size_t ap = strlen(s.apPass);
  if (ap < 8 || ap > 63) { err = "apPass"; return false; }
  if (strlen(s.adminPass) < 4) { err = "adminPass"; return false; }
  if (s.logLevel > LOG_DEBUG) s.logLevel = LOG_INFO;
  return true;
}

void load() {
  defaults(cur);
  Preferences p;
  size_t n = 0;
  Settings* tmp = new Settings;
  defaults(*tmp);
  bool ok = false;
  if (p.begin(NS, true)) {
    n = p.getBytesLength("s");
    // Same or older layout: read what is stored over the defaults (fields appended later keep
    // their defaults). A newer layout (a downgrade) is not read.
    if (n >= SIZE_V2 && n <= sizeof(Settings)) {
      uint16_t ver = 0;
      ok = p.getBytes("s", tmp, n) == n;
      ver = tmp->version;
      ok = ok && ver >= 2 && ver <= VERSION;
      if (ok && ver < VERSION) LOGI("cfg", "settings from version %u upgraded", ver);
      tmp->version = VERSION;
    }
    p.end();
  }
  String err;
  if (ok && validate(*tmp, err)) {
    cur = *tmp;
    LOGI("cfg", "settings loaded");
  } else {
    // Nothing stored after a factory reset: the identity only. Unreadable: the link too.
    bool lost = n > 0;
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

void factoryReset(bool wipeIdentity) {
  Preferences p;
  if (p.begin(NS, false)) {
    String ca = p.getString("ca", "");
    p.clear();                                   // load() then restores the identity only
    if (!wipeIdentity && ca.length()) p.putString("ca", ca);
    p.end();
  }
  if (p.begin(IDENT, false)) {
    if (wipeIdentity) p.clear();
    else { p.remove("net"); p.remove("mode"); }  // the customer's Wi-Fi goes with the reset
    p.end();
  }
  for (const char* ns : {"rt", "loc", "rules", "ota"})
    if (p.begin(ns, false)) { p.clear(); p.end(); }
  if (wipeIdentity && p.begin("label", false)) { p.clear(); p.end(); }
  LOGW("cfg", wipeIdentity ? "erased, identity included" : "factory reset (identity kept)");
}

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
  for (char c : qr) if ((uint8_t)c < 0x21) return false;
  Preferences p;
  if (!p.begin("label", false)) return false;
  if (qr.length()) p.putString("qr", qr); else p.remove("qr");
  if (serial.length()) p.putString("sn", serial); else p.remove("sn");
  p.end();
  LOGI("cfg", "product label %s", qr.length() ? "stored" : "cleared");
  return true;
}

String caPem() {
  Preferences p;
  if (!p.begin(NS, true)) return String();
  String v = p.getString("ca", "");
  p.end();
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

uint8_t modbusAddress() {
  if (!cur.mbAddrFromDip) return cur.mbAddr;
  uint8_t a = inputs::address();
  return a <= 247 ? a : 0;
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

String apSsid() { return "TiTi-R8-" + macSuffix(); }

String defaultClientId() {
  return cur.deviceId[0] ? String(cur.deviceId) : String("titi-r8-") + macSuffix();
}

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
  sy["adminDefault"] = strcmp(s.adminPass, "admin") == 0;
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
        // A changed or cleared network forgets its old password.
        if (strcmp(ssid, s.nets[i].ssid) != 0) s.nets[i].pass[0] = 0;
        copyStr(s.nets[i].ssid, sizeof s.nets[i].ssid, ssid);
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
    if (j["host"].is<const char*>() && strcmp(j["host"], v.host) != 0) v.pass[0] = 0;
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
}  // namespace settings
