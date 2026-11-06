#include "rules.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>
#include "board.h"
#include "local.h"
#include "log.h"
#include "net.h"
#include "relays.h"

namespace rules {
namespace {
constexpr size_t MAX_RULES_JSON = 12 * 1024;
constexpr uint8_t MAX_RUNS = 8;

SemaphoreHandle_t mtx = nullptr;
JsonDocument* doc = nullptr;        // the current rules message
volatile bool fresh = false;
char ver[16] = "";
uint8_t n = 0;
int32_t offsetMin = 210;            // Iran; replaced by utc_offset_min
uint32_t offlineAfterMs = 60000;
uint32_t lastReachable = 0;
int32_t lastMinute = -1;
uint8_t lastValues = 0;
uint32_t valueSince[32] = {0};      // value triggers held since (for_s), per rule
bool valueFired[32] = {false};

struct Run { int16_t rule; uint8_t step; uint32_t at; };
Run runs[MAX_RUNS];

// --- Jalali calendar (hub/jalali.py) ---------------------------------------------------------
void toJalali(int gy, int gm, int gd, int& jy, int& jm, int& jd) {
  static const int gdm[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  long gy2 = gm > 2 ? gy + 1 : gy;
  long days = 355666 + 365L * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400 + gd + gdm[gm - 1];
  jy = -1595 + 33 * (days / 12053);
  days %= 12053;
  jy += 4 * (days / 1461);
  days %= 1461;
  if (days > 365) {
    jy += (days - 1) / 365;
    days = (days - 1) % 365;
  }
  if (days < 186) { jm = 1 + days / 31; jd = 1 + days % 31; }
  else { jm = 7 + (days - 186) / 30; jd = 1 + (days - 186) % 30; }
}

struct Day {
  int jy, jm, jd;
  int weekday;      // 0 = Saturday
  bool holiday;
  int minute;       // minute of the day, local
};

bool parseJdate(const char* s, int& y, int& m, int& d) {
  return s && sscanf(s, "%d%*[/-]%d%*[/-]%d", &y, &m, &d) == 3;
}

int hhmm(const char* s) {
  int h, m;
  return (s && sscanf(s, "%d:%d", &h, &m) == 2) ? h * 60 + m : -1;
}

bool inList(JsonVariantConst list, int v) {
  if (list.isNull()) return true;
  for (JsonVariantConst x : list.as<JsonArrayConst>()) if (x.as<int>() == v) return true;
  return false;
}

bool dayOk(JsonObjectConst c, const Day& d) {
  if (!inList(c["weekdays"], d.weekday)) return false;
  if (!inList(c["months"], d.jm)) return false;
  if (!inList(c["month_days"], d.jd)) return false;
  int y, m, dd;
  long today = d.jy * 10000L + d.jm * 100 + d.jd;
  if (parseJdate(c["start"], y, m, dd) && today < y * 10000L + m * 100 + dd) return false;
  if (parseJdate(c["end"], y, m, dd) && today > y * 10000L + m * 100 + dd) return false;
  const char* h = c["holidays"] | "any";
  if (!strcmp(h, "skip") && d.holiday) return false;
  if (!strcmp(h, "only") && !d.holiday) return false;
  return true;
}

bool between(JsonVariantConst b, int minute) {
  if (b.isNull()) return true;
  int s = hhmm(b[0]), e = hhmm(b[1]);
  if (s < 0 || e < 0) return true;
  return s <= e ? (minute >= s && minute < e) : (minute >= s || minute < e);
}

// The module's own values by tag key.
bool valueOf(const char* tag, JsonDocument& out) {
  if (!strncmp(tag, "relay_", 6)) {
    int r = atoi(tag + 6) - 1;
    if (r < 0 || r >= RELAY_COUNT || !relays::fitted(r)) return false;
    out.set((bool)((relays::state() >> r) & 1));
    return true;
  }
  if (!strcmp(tag, "uptime_s")) { out.set(millis() / 1000); return true; }
  return false;
}

bool compare(const char* tag, const char* op, JsonVariantConst want) {
  JsonDocument v;
  if (!valueOf(tag, v)) return false;
  double a, b;
  if (v.is<bool>()) a = v.as<bool>(); else a = v.as<double>();
  if (want.is<bool>()) b = want.as<bool>(); else b = want.as<double>();
  if (!strcmp(op, "eq")) return a == b;
  if (!strcmp(op, "ne")) return a != b;
  if (!strcmp(op, "lt")) return a < b;
  if (!strcmp(op, "le")) return a <= b;
  if (!strcmp(op, "gt")) return a > b;
  if (!strcmp(op, "ge")) return a >= b;
  return false;
}

bool conditionsOk(JsonObjectConst rule, const Day& d) {
  JsonObjectConst w = rule["window"];
  if (w) {
    if (!dayOk(w, d) || !between(w["between"], d.minute)) return false;
  }
  for (JsonObjectConst c : rule["conditions"].as<JsonArrayConst>())
    if (!compare(c["tag"] | "", c["op"] | "", c["value"])) return false;
  return true;
}

void start(int idx, const char* why) {
  for (Run& r : runs) {
    if (r.rule >= 0 && r.rule == idx) return;            // already running
  }
  for (Run& r : runs) {
    if (r.rule < 0) {
      r = {(int16_t)idx, 0, millis()};
      JsonObjectConst rule = (*doc)["rules"][idx];
      LOGI("rules", "offline rule \"%s\" (%s)", (const char*)(rule["name"] | "?"), why);
      return;
    }
  }
}

// Steps through each running rule: writes now, delays wait.
void step() {
  uint32_t now = millis();
  for (Run& r : runs) {
    if (r.rule < 0 || (int32_t)(now - r.at) < 0) continue;
    JsonArrayConst actions = (*doc)["rules"][r.rule]["actions"];
    while (r.step < actions.size()) {
      JsonObjectConst a = actions[r.step++];
      const char* type = a["type"] | "";
      if (!strcmp(type, "delay")) {
        r.at = now + (uint32_t)((a["seconds"] | 0.0f) * 1000);
        break;
      }
      if (!strcmp(type, "write")) {
        const char* tag = a["tag"] | "";
        int relay = strncmp(tag, "relay_", 6) ? -1 : atoi(tag + 6) - 1;
        JsonVariantConst v = a["value"];
        bool on = v.is<bool>() ? v.as<bool>() : v.as<double>() != 0;
        if (relay >= 0 && relay < RELAY_COUNT && relays::fitted(relay)) {
          relays::set(relay, on, relays::SRC_RULE);
          char by[24];
          snprintf(by, sizeof by, "rule:%ld", (long)((*doc)["rules"][r.rule]["id"] | 0L));
          local::log(relay, on, by);
        }
      }
    }
    if (r.step >= actions.size() && (int32_t)(now - r.at) >= 0) r.rule = -1;
  }
}

bool today(Day& d) {
  if (!net::timeValid()) return false;
  time_t t = time(nullptr) + (time_t)offsetMin * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  toJalali(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, d.jy, d.jm, d.jd);
  d.weekday = (tm.tm_wday + 1) % 7;
  d.minute = tm.tm_hour * 60 + tm.tm_min;
  char iso[11];
  strftime(iso, sizeof iso, "%Y-%m-%d", &tm);
  d.holiday = false;
  for (JsonVariantConst h : (*doc)["holidays"].as<JsonArrayConst>())
    if (!strcmp(h | "", iso)) { d.holiday = true; break; }
  return true;
}

void load(JsonDocument* d) {
  strlcpy(ver, (*d)["version"] | "", sizeof ver);
  n = (*d)["rules"].size();
  offsetMin = (*d)["utc_offset_min"] | 210;
  offlineAfterMs = ((*d)["offline_after_s"] | 60) * 1000UL;
  for (Run& r : runs) r.rule = -1;
  for (bool& f : valueFired) f = false;
  for (uint32_t& t : valueSince) t = 0;
}
}  // namespace

void begin() {
  mtx = xSemaphoreCreateMutex();
  for (Run& r : runs) r.rule = -1;
  Preferences p;
  if (!p.begin("rules", true)) return;
  size_t len = p.getBytesLength("j");
  if (len && len <= MAX_RULES_JSON) {
    char* buf = (char*)malloc(len + 1);
    if (buf) {
      p.getBytes("j", buf, len);
      buf[len] = 0;
      doc = new JsonDocument;
      if (deserializeJson(*doc, buf, len)) { delete doc; doc = nullptr; }
      else load(doc);
      free(buf);
    }
  }
  p.end();
  if (doc) LOGI("rules", "%u offline rules (version %s)", n, ver);
}

void apply(const char* json, size_t len) {
  if (len > MAX_RULES_JSON) { LOGW("rules", "rules message too large"); return; }
  auto* d = new JsonDocument;
  if (deserializeJson(*d, json, len) || strcmp((*d)["type"] | "", "rules") != 0) {
    delete d;
    LOGW("rules", "rules message unreadable");
    return;
  }
  // A module without NTP takes the server's clock.
  const char* st = (*d)["server_time"] | "";
  if (*st && !net::timeValid()) {
    struct tm tm = {};
    if (sscanf(st, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
               &tm.tm_min, &tm.tm_sec) == 6) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      setenv("TZ", "UTC0", 1);
      tzset();
      struct timeval tv = {mktime(&tm), 0};
      settimeofday(&tv, nullptr);
      LOGI("rules", "clock set from the server");
    }
  }
  bool same = !strcmp((*d)["version"] | "", ver) && doc;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!same) {
    delete doc;
    doc = d;
    load(doc);
    Preferences p;
    if (p.begin("rules", false)) { p.putBytes("j", json, len); p.end(); }
    LOGI("rules", "%u offline rules received (version %s)", n, ver);
  } else {
    delete d;
  }
  xSemaphoreGive(mtx);
}

void loop(bool serverReachable) {
  uint32_t now = millis();
  if (serverReachable || !lastReachable) lastReachable = now ? now : 1;
  if (!doc || xSemaphoreTake(mtx, 0) != pdTRUE) return;
  bool cutOff = !serverReachable && now - lastReachable >= offlineAfterMs;
  if (!cutOff) {
    for (Run& r : runs) r.rule = -1;      // the server runs them again
    lastMinute = -1;
    xSemaphoreGive(mtx);
    return;
  }
  Day d;
  if (today(d)) {
    JsonArrayConst list = (*doc)["rules"];
    bool newMinute = d.minute != lastMinute;
    uint8_t vals = relays::state();
    bool valuesChanged = vals != lastValues;
    lastValues = vals;
    for (uint8_t i = 0; i < list.size() && i < 32; i++) {
      JsonObjectConst rule = list[i];
      JsonObjectConst tr = rule["trigger"];
      const char* type = tr["type"] | "";
      bool fire = false;
      if (newMinute && !strcmp(type, "time")) {
        fire = hhmm(tr["at"]) == d.minute && dayOk(tr, d);
      } else if (newMinute && !strcmp(type, "interval")) {
        int every = tr["every_min"] | 0;
        fire = every > 0 && d.minute % every == 0 && between(tr["between"], d.minute) && dayOk(tr, d);
      } else if (!strcmp(type, "value") && (valuesChanged || newMinute || valueSince[i])) {
        bool met = compare(tr["tag"] | "", tr["op"] | "", tr["value"]);
        uint32_t holdMs = (tr["for_s"] | 0) * 1000UL;
        if (!met) { valueSince[i] = 0; valueFired[i] = false; }
        else if (!valueFired[i]) {
          if (!valueSince[i]) valueSince[i] = now ? now : 1;
          if (now - valueSince[i] >= holdMs) { fire = true; valueFired[i] = true; }
        }
      }
      if (fire && conditionsOk(rule, d)) start(i, type);
    }
    lastMinute = d.minute;
  }
  step();
  xSemaphoreGive(mtx);
}

uint8_t count() { return n; }
const char* version() { return ver; }
}  // namespace rules
