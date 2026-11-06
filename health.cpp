#include "health.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "log.h"
#include "relays.h"

namespace health {
namespace {
constexpr uint32_t WDT_TIMEOUT_MS = 8000;
constexpr uint32_t STABLE_AFTER_MS = 60000;      // this long up = the boot was good
constexpr uint8_t CRASH_LIMIT = 3;
constexpr uint32_t LOW_HEAP = 16 * 1024;
constexpr uint32_t LOW_HEAP_FOR_MS = 30000;
constexpr uint32_t HEAP_REPORT_MS = 10UL * 60 * 1000;

bool safe = false;
bool stableMarked = false;
uint32_t boots = 0;
uint8_t badBoots = 0;
uint32_t lowHeapSince = 0;
uint32_t lastHeapReport = 0;
esp_reset_reason_t reason = ESP_RST_UNKNOWN;

bool abnormal(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}
}  // namespace

void begin() {
  reason = esp_reset_reason();
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);

  Preferences p;
  if (p.begin("hl", false)) {
    boots = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", boots);
    badBoots = abnormal(reason) ? p.getUChar("bad", 0) + 1 : 0;
    p.putUChar("bad", badBoots);
    p.end();
  }
  safe = badBoots >= CRASH_LIMIT;
  if (abnormal(reason)) LOGW("health", "abnormal reset: %s (%u in a row)", resetReason(), badBoots);
  if (safe) LOGE("health", "SAFE MODE: %u abnormal resets in a row", badBoots);
}

void watchThisTask() { esp_task_wdt_add(nullptr); }
void feed() { esp_task_wdt_reset(); }

void loop() {
  uint32_t now = millis();
  if (!stableMarked && now > STABLE_AFTER_MS) {
    stableMarked = true;
    if (badBoots) {
      Preferences p;
      if (p.begin("hl", false)) { p.putUChar("bad", 0); p.end(); }
    }
  }
  uint32_t heap = ESP.getFreeHeap();
  if (heap < LOW_HEAP) {
    if (!lowHeapSince) {
      lowHeapSince = now ? now : 1;
      LOGW("health", "free heap low: %lu bytes", (unsigned long)heap);
    } else if (now - lowHeapSince > LOW_HEAP_FOR_MS) {
      restart("free heap stayed low");
    }
  } else {
    lowHeapSince = 0;
  }
  if (now - lastHeapReport >= HEAP_REPORT_MS) {
    lastHeapReport = now;
    LOGD("health", "heap free %lu, min %lu, largest %lu", (unsigned long)heap,
         (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  }
}

bool safeMode() { return safe; }
uint32_t bootCount() { return boots; }

const char* resetReason() {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void restart(const char* why) {
  LOGW("health", "restart: %s", why);
  relays::saveNow();
  Serial.flush();
  delay(200);
  ESP.restart();
}
}  // namespace health
