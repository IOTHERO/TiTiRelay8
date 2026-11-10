#include "console.h"
#include <Arduino.h>
#include "board.h"
#include "health.h"
#include "settings.h"

namespace console {
namespace {
char line[200];
uint8_t len = 0;
Settings* draft = nullptr;      // edits wait here until "save"

Settings& d() {
  if (!draft) draft = new Settings(settings::cur);
  return *draft;
}

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

void reply(const char* s) { Serial.printf("> %s\n", s); }

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
    reply("id, secret, server <host> [port] [tls], api <url>, wifi <ssid> [pass], mode wifi|rs485, qr <text>, serial <sn>, show, save, restart, factory yes, wipe yes");
  } else if (!strcmp(cmd, "show")) {
    show();
  } else if (!strcmp(cmd, "id") && arg && *arg) {
    strlcpy(d().deviceId, arg, sizeof d().deviceId);
    strlcpy(platformServer().user, arg, sizeof platformServer().user);
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
    v.port = port ? port : (v.tls ? 8883 : 1883);
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
    // The production console erases the identity too, so a board can be provisioned again
    // (hub-provision refuses a board that still holds a device id). The customer's factory
    // reset (portal, config switch) keeps it.
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
