// TiTi Relay-8: 8-channel relay output module (ESP32-WROOM-32) for the TiTiSwitch platform.
//
// Power-up with the config switch (DIP 8) ON  -> config portal, Wi-Fi "TiTi-R8-xxxx",
//                                                 http://192.168.1.100/
// Power-up with it OFF                         -> the link chosen in the portal:
//   Wi-Fi  : MQTT to the platform (native device contract) or a generic broker
//   RS-485 : Modbus RTU slave, address from DIP AD1..AD7 (or set in the portal)
//
// Board: "ESP32 Dev Module", Partition scheme: "Minimal SPIFFS (1.9MB APP with OTA)".
// Libraries: ArduinoJson 7, ESP Async WebServer 3.x, Async TCP 3.x. See README.md.

#include <esp_mac.h>
#include "board.h"
#include "health.h"
#include "inputs.h"
#include "leds.h"
#include "cloud.h"
#include "console.h"
#include "hubapi.h"
#include "local.h"
#include "log.h"
#include "modbus.h"
#include "net.h"
#include "portal.h"
#include "relays.h"
#include "rules.h"
#include "settings.h"

namespace {
enum RunMode : uint8_t { RUN_PORTAL, RUN_WIFI, RUN_RS485 };
RunMode runMode = RUN_PORTAL;
bool lossApplied = false;
uint32_t cfgOnSince = 0;
bool pairingSent = false;
uint8_t cfgToggles = 0;
uint32_t cfgToggleStart = 0;
bool cfgLast = false;

void banner() {
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.printf(" %s  %s  fw %s\n", FW_NAME, FW_MODEL, FW_VERSION);
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  Serial.printf(" MAC %02X:%02X:%02X:%02X:%02X:%02X  reset: %s  boot #%lu\n", m[0], m[1], m[2], m[3],
                m[4], m[5], health::resetReason(), (unsigned long)health::bootCount());
  Serial.println(F("=============================================="));
}

// Link loss (no Modbus master / MQTT down for linkTimeoutS): the configured safe action, once.
void linkLossCheck(uint32_t now) {
  const Settings& s = settings::cur;
  if (!s.linkTimeoutS || s.linkLoss == LOSS_HOLD) return;
  uint32_t lastOk;
  if (runMode == RUN_RS485) {
    lastOk = modbus::lastFrameMs();
  } else {
    uint32_t down = cloud::downSinceMs();
    lastOk = down ? down : now;
  }
  bool lost = now - lastOk > s.linkTimeoutS * 1000UL;   // lastOk 0 = since power-up
  if (lost && !lossApplied) {
    lossApplied = true;
    LOGW("main", "link lost for %u s: %s", s.linkTimeoutS,
         s.linkLoss == LOSS_ALL_OFF ? "all outputs OFF" : "preset outputs");
    relays::Cmd c = {};
    c.op = s.linkLoss == LOSS_ALL_OFF ? relays::OP_ALL_OFF : relays::OP_MASK;
    c.mask = s.linkLossMask;
    c.src = relays::SRC_LINK;
    c.force = true;
    relays::post(c);
  } else if (!lost && lossApplied) {
    lossApplied = false;
    LOGI("main", "link restored");
  }
}

void runLed(uint32_t now) {
  if (health::safeMode()) return leds::run(leds::RUN_FAULT);
  if (runMode == RUN_RS485) {
    if (modbus::address() == 0) return leds::run(leds::RUN_FAULT);
    uint32_t last = modbus::lastFrameMs();
    return leds::run(last && now - last < 5000 ? leds::RUN_OK : leds::RUN_NO_LINK);
  }
  leds::run(cloud::connected() ? leds::RUN_OK : leds::RUN_NO_LINK);
}

// Wi-Fi mode: the config switch turned ON while running and held 3 s = pairing request.
void pairingCheck(uint32_t now) {
  if (!inputs::configOn()) {
    cfgOnSince = 0;
    pairingSent = false;
    return;
  }
  if (!cfgOnSince) cfgOnSince = now ? now : 1;
  if (!pairingSent && now - cfgOnSince >= 3000) {
    pairingSent = true;
    cloud::pairing();
    leds::identify(3000);
    LOGI("main", "config switch ON: turn it OFF again; power-cycle with it ON for the portal");
  }
}
// Config portal: the config switch turned OFF and ON again 5 times within 15 s = factory reset
// (the way back when the admin password is lost).
void factoryGesture(uint32_t now) {
  bool on = inputs::configOn();
  if (on == cfgLast) return;
  cfgLast = on;
  if (!on) return;
  if (!cfgToggles || now - cfgToggleStart > 15000) { cfgToggles = 0; cfgToggleStart = now; }
  if (++cfgToggles >= 5) {
    LOGW("main", "config switch toggled 5 times: factory reset");
    leds::identify(3000);
    settings::factoryReset();
    health::restart("factory reset with the config switch");
  } else if (cfgToggles >= 3) {
    LOGW("main", "factory reset in %u more toggles of the config switch", 5 - cfgToggles);
  }
}
}  // namespace

void setup() {
  logx::begin(115200);
  leds::begin();
  health::begin();
  inputs::begin();
  settings::load();
  logx::setLevel(settings::cur.logLevel);
  banner();
  relays::onResult(cloud::ackResult);
  relays::begin(health::safeMode());
  local::begin();
  rules::begin();
  health::watchThisTask();
  cfgLast = inputs::configOn();

  if (inputs::configAtBoot() || settings::cur.mode == MODE_UNSET) {
    if (!inputs::configAtBoot()) LOGW("main", "not configured yet: starting the config portal");
    runMode = RUN_PORTAL;
    portal::begin();
  } else if (settings::cur.mode == MODE_WIFI) {
    runMode = RUN_WIFI;
    net::begin(false);
    hubapi::begin();
    cloud::begin();
  } else {
    runMode = RUN_RS485;
    modbus::begin();
  }
  LOGI("main", "running: %s", runMode == RUN_PORTAL ? "config portal"
                               : runMode == RUN_WIFI ? "Wi-Fi / MQTT" : "RS-485 / Modbus RTU");
}

void loop() {
  uint32_t now = millis();
  health::feed();
  inputs::loop();
  relays::loop();
  leds::loop();
  health::loop();
  logx::flushRepeats();
  console::loop();
  switch (runMode) {
    case RUN_PORTAL:
      portal::loop();
      factoryGesture(now);
      break;
    case RUN_WIFI:
      net::loop();
      cloud::loop();
      local::loop();
      rules::loop(cloud::connected());
      linkLossCheck(now);
      pairingCheck(now);
      runLed(now);
      break;
    case RUN_RS485:
      linkLossCheck(now);
      runLed(now);
      break;
  }
  delay(2);
}
