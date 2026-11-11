// =====================================================================================
//  cloud.cpp — اتصال MQTT به سرور (پلتفرم TiTiSwitch یا بروکر عمومی)
// =====================================================================================
//
//  فهرست تاپیک‌ها و قالب پیام‌ها در core.h (بخش cloud) آمده است. نکته‌های مهم این فایل:
//
//  ● چند سرور: سرورها به ترتیب امتحان می‌شوند؛ پس از شکست، سرور بعدی. اگر اتصالی که مدتی
//    پایدار بود قطع شود، اول دوباره همان سرور امتحان می‌شود.
//  ● تلاش مجدد طبق قرارداد سرور: اولین تلاش بعد از ۰ تا ۱۰ ثانیه تصادفی، بعد دو برابر تا ۵ دقیقه
//    (±۲۰٪)، تا با برگشت برق هزاران دستگاه هم‌زمان به سرور نچسبند.
//  ● فرمان تکراری: MQTT با QoS 1 ممکن است یک پیام را دو بار برساند؛ شناسهٔ فرمان‌های اخیر
//    نگه داشته می‌شود و فرمان تکراری فقط «دوباره جواب داده» می‌شود، نه دوباره اجرا.
//  ● حد پیام: سرور بیش از ۵ پیام در ثانیه از هر دستگاه را دور می‌ریزد؛ ما حداکثر ۴ می‌فرستیم.
//  ● TLS: تاریخ گواهی بررسی می‌شود پس تا آماده شدن ساعت (NTP) صبر می‌کنیم. گواهی CA از منو یا
//    فایل /ca.pem (settings::caPem) می‌آید.
//  ● هر Callback رویداد MQTT در Task خود کتابخانه اجرا می‌شود؛ فقط کار سبک می‌کنیم و
//    تغییر رله‌ها را به صف رله‌ها می‌سپاریم.
// =====================================================================================
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <mqtt_client.h>
#include "core.h"

namespace cloud {
namespace {
enum State : uint8_t { IDLE, WAIT, CONNECTING, UP };
State state = IDLE;
esp_mqtt_client_handle_t client = nullptr;
int8_t srv = -1;
uint8_t attempt = 0;
uint32_t timeWaitStart = 0;
uint32_t nextAt = 0;
uint32_t startedAt = 0;
uint32_t upAt = 0;
uint32_t downAt = 1;
uint32_t lastTelemetry = 0;
uint32_t lastChanges = 0xFFFFFFFF;
volatile bool evConnected = false;
volatile bool evDown = false;
uint32_t rx = 0, tx = 0;   // statistics only
char err[64] = "";
String caCache;
bool caLoaded = false;

int8_t testIdx = -1;
String testText;

char tBase[112], tTelemetry[128], tState[128], tAck[128], tPairing[128], tCmd[128];
char tConfig[128], tRules[128], tOta[128];
char clientId[64];
bool generic = false;
uint8_t qos = 1;

const ServerCfg& server() { return settings::cur.servers[srv]; }

void buildTopics() {
  const ServerCfg& v = server();
  generic = v.proto == PROTO_GENERIC;
  char* base = tBase;
  if (generic) {
    snprintf(tBase, sizeof tBase, "%s/", v.prefix);
  } else {
    snprintf(tBase, sizeof tBase, "%sd/%s/", v.prefix, settings::cur.deviceId);
  }
  snprintf(tTelemetry, sizeof tTelemetry, "%stelemetry", base);
  snprintf(tState, sizeof tState, "%s%s", base, generic ? "status" : "state");
  snprintf(tAck, sizeof tAck, "%sack", base);
  snprintf(tPairing, sizeof tPairing, "%spairing", base);
  snprintf(tCmd, sizeof tCmd, "%scmd", base);
  snprintf(tConfig, sizeof tConfig, "%sconfig", base);
  snprintf(tRules, sizeof tRules, "%srules", base);
  snprintf(tOta, sizeof tOta, "%sota", base);
  if (v.clientId[0]) strlcpy(clientId, v.clientId, sizeof clientId);
  else strlcpy(clientId, settings::defaultClientId().c_str(), sizeof clientId);
  qos = v.qos;
}

const char* offlineMsg() { return generic ? "offline" : "{\"state\":\"offline\"}"; }
// Platform: the firmware version and, for local control, the private address on the home Wi-Fi.
String onlineMsg() {
  if (generic) return "online";
  JsonDocument d;
  d["state"] = "online";
  d["firmware"] = FW_VERSION;
  String a = settings::cur.localEnabled ? local::addr() : String();
  if (a.length()) d["local_addr"] = a;
  String s;
  serializeJson(d, s);
  return s;
}

void publish(const char* topic, const char* payload, int q, bool retain) {
  if (!client || state != UP) return;
  if (esp_mqtt_client_publish(client, topic, payload, 0, q, retain) >= 0) {
    tx++;
    leds::data();
  }
}

void publishJson(const char* topic, JsonDocument& doc) {
  String s;
  serializeJson(doc, s);
  publish(topic, s.c_str(), qos, false);
}

void sendAck(const char* id, const char* status, int relay, bool value, const char* error) {
  JsonDocument d;
  d["command_id"] = id;
  d["status"] = status;
  if (relay >= 0) d["value"] = value;
  if (error) d["error"] = error;
  publishJson(tAck, d);
}

void sendTelemetry() {
  JsonDocument d;
  uint64_t ms = net::epochMs();
  if (ms) d["ts"] = ms;
  JsonObject v = d["values"].to<JsonObject>();
  uint8_t st = relays::state();
  char key[10];
  for (uint8_t r = 0; r < RELAY_COUNT; r++) {
    if (!relays::fitted(r)) continue;
    snprintf(key, sizeof key, "relay_%u", r + 1);
    v[key] = (bool)((st >> r) & 1);
  }
  v["uptime_s"] = millis() / 1000;
  publishJson(tTelemetry, d);
  lastTelemetry = millis();
}

// Command ids already seen and how they ended (0 received, 1 done, 2 failed).
struct Seen { char id[40]; uint8_t status; int8_t relay; bool value; };
Seen seen[MQTT_SEEN_IDS] = {};
uint8_t seenNext = 0;
portMUX_TYPE seenMux = portMUX_INITIALIZER_UNLOCKED;

int findSeen(const char* id) {
  for (uint8_t i = 0; i < MQTT_SEEN_IDS; i++) if (seen[i].id[0] && !strcmp(seen[i].id, id)) return i;
  return -1;
}

void remember(const char* id, uint8_t status, int8_t relay, bool value) {
  portENTER_CRITICAL(&seenMux);
  int i = findSeen(id);
  if (i < 0) {
    i = seenNext;
    seenNext = (seenNext + 1) % MQTT_SEEN_IDS;
    strlcpy(seen[i].id, id, sizeof seen[i].id);
  }
  seen[i].status = status;
  seen[i].relay = relay;
  seen[i].value = value;
  portEXIT_CRITICAL(&seenMux);
}

// "2026-10-27T10:00:00Z", "...00.123+03:30" -> epoch seconds; 0 if it cannot be read.
int64_t parseIso(const char* s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  const char* p = s + 19;
  if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
  int offMin = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    sscanf(p + 1, "%2d:%2d", &oh, &om);
    offMin = (oh * 60 + om) * (*p == '-' ? -1 : 1);
  }
  // days from civil (Howard Hinnant)
  y -= mo <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + se - offMin * 60;
}

int relayFromTag(const char* tag) {
  if (!tag || strncmp(tag, "relay_", 6) != 0) return -1;
  int n = atoi(tag + 6);
  return (n >= 1 && n <= RELAY_COUNT) ? n - 1 : -1;
}

// true/false, 1/0, "on"/"off", "true"/"false"; -1 if none of these.
int boolValue(JsonVariantConst v) {
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<long>()) { long n = v.as<long>(); return n == 0 ? 0 : n == 1 ? 1 : -1; }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcmp(s, "1")) return 1;
    if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcmp(s, "0")) return 0;
  }
  return -1;
}

// Runs in the MQTT task: checks the command and hands it to the main loop.
void onCommand(const char* data, int len) {
  JsonDocument d;
  if (deserializeJson(d, data, len)) {
    LOGW("mqtt", "command is not JSON");
    return;
  }
  JsonObjectConst o = d.as<JsonObjectConst>();
  if (o["type"].is<const char*>()) return;       // rules / config: not used by this module
  const char* id = o["command_id"] | "";
  if (*id) {
    int i = findSeen(id);
    if (i >= 0) {                                   // redelivered (QoS 1): answer, do not repeat
      Seen x = seen[i];
      static const char* names[] = {"received", "done", "failed"};
      sendAck(id, names[x.status], x.status == 1 ? x.relay : -1, x.value,
              x.status == 2 ? "failed" : nullptr);
      LOGD("mqtt", "command repeated, not carried out again");
      return;
    }
  }
  if (*id || o["tag"].is<const char*>()) {
    int r = relayFromTag(o["tag"]);
    int v = boolValue(o["value"]);
    if (r < 0 || v < 0) {
      if (*id) {
        remember(id, 2, -1, false);
        sendAck(id, "failed", -1, false, r < 0 ? "unknown_tag" : "bad_value");
      }
      LOGW("mqtt", "command refused: %s", r < 0 ? "unknown tag" : "bad value");
      return;
    }
    if (o["expires_at"].is<const char*>() && net::timeValid()) {
      int64_t exp = parseIso(o["expires_at"]);
      if (exp && exp < (int64_t)time(nullptr)) {
        if (*id) {
          remember(id, 2, r, false);
          sendAck(id, "failed", r, (relays::state() >> r) & 1, "expired");
        }
        LOGW("mqtt", "command for relay %d expired, not executed", r + 1);
        return;
      }
    }
    relays::Cmd c = {};
    c.op = relays::OP_SET;
    c.relay = r;
    c.value = v;
    c.src = relays::SRC_MQTT;
    float pulse = o["pulse_s"] | 0.0f;
    if (pulse > 0 && pulse <= 86400) c.pulseMs = (uint32_t)(pulse * 1000);
    strlcpy(c.id, id, sizeof c.id);
    if (!relays::post(c)) {
      if (*id) sendAck(id, "failed", r, (relays::state() >> r) & 1, "busy");
      return;
    }
    if (*id) {
      remember(id, 0, r, v);
      sendAck(id, "received", -1, false, nullptr);
    }
    return;
  }
  // Generic form: {"relay_1": true, "relay_3": false} or {"mask": 5}
  if (o["mask"].is<long>()) {
    relays::setMask((uint8_t)o["mask"].as<long>(), relays::SRC_MQTT);
    return;
  }
  uint8_t n = 0;
  for (JsonPairConst kv : o) {
    int r = relayFromTag(kv.key().c_str());
    int v = boolValue(kv.value());
    if (r >= 0 && v >= 0) { relays::set(r, v, relays::SRC_MQTT); n++; }
  }
  if (!n) LOGW("mqtt", "command without a known relay key");
}

bool topicIs(esp_mqtt_event_handle_t e, const char* t) {
  return e->topic_len == (int)strlen(t) && !strncmp(e->topic, t, e->topic_len);
}

void onEvent(void*, esp_event_base_t, int32_t id, void* data) {
  auto* e = static_cast<esp_mqtt_event_handle_t>(data);
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      esp_mqtt_client_subscribe(e->client, tCmd, qos);
      if (!generic) {
        esp_mqtt_client_subscribe(e->client, tConfig, 1);
        esp_mqtt_client_subscribe(e->client, tRules, 1);
        esp_mqtt_client_subscribe(e->client, tOta, 1);
      }
      evConnected = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      evDown = true;
      break;
    case MQTT_EVENT_ERROR:
      if (e->error_handle) {
        auto* h = e->error_handle;
        if (h->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          int rc = h->connect_return_code;
          snprintf(err, sizeof err, "refused (%s)",
                   rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME || rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                       ? "username/password"
                   : rc == MQTT_CONNECTION_REFUSE_ID_REJECTED ? "client id"
                                                                : "server unavailable");
        } else if (h->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          snprintf(err, sizeof err, "network/TLS error (tls 0x%x, sock %d)",
                   (unsigned)h->esp_tls_last_esp_err, h->esp_transport_sock_errno);
        } else {
          snprintf(err, sizeof err, "error type %d", (int)h->error_type);
        }
      }
      evDown = true;
      break;
    case MQTT_EVENT_DATA:
      rx++;
      leds::data();
      if (e->data_len != e->total_data_len) {
        LOGW("mqtt", "message too large, ignored");
        break;
      }
      if (topicIs(e, tCmd)) {
        onCommand(e->data, e->data_len);
      } else if (topicIs(e, tRules)) {
        rules::apply(e->data, e->data_len);
      } else if (topicIs(e, tConfig)) {
        local::applyConfig(e->data, e->data_len);       // کلید کنترل محلی
      } else if (topicIs(e, tOta)) {
        hubapi::offerOta(e->data, e->data_len);         // پیشنهاد Firmware تازه
      }
      break;
    default:
      break;
  }
}

void teardown() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  evConnected = false;
  evDown = false;
}

int8_t nextServer(int8_t from) {
  for (uint8_t k = 1; k <= MAX_SERVERS; k++) {
    int8_t i = (from + k + MAX_SERVERS) % MAX_SERVERS;
    if (settings::cur.servers[i].enabled) return i;
  }
  return -1;
}

uint32_t retryDelay() {
  uint32_t ms;
  if (attempt == 0) {
    ms = esp_random() % MQTT_RETRY_FIRST_MAX_MS;
  } else {
    uint32_t base = MQTT_RETRY_FIRST_MAX_MS << (attempt > 5 ? 5 : attempt - 1);
    if (base > MQTT_RETRY_CAP_MS) base = MQTT_RETRY_CAP_MS;
    ms = base * (80 + esp_random() % 41) / 100;   // +-20 %
  }
  if (attempt < 255) attempt++;
  return ms;
}

void scheduleRetry(bool wasStable) {
  teardown();
  state = WAIT;
  if (testIdx >= 0) {
    testText = String("failed: ") + (err[0] ? err : "timeout");
    testIdx = -1;
    state = IDLE;
    return;
  }
  if (wasStable) attempt = 0;
  // The next server only after a failed attempt; a dropped good session first retries the same.
  if (!wasStable) srv = nextServer(srv);
  uint32_t ms = retryDelay();
  nextAt = millis() + ms;
  LOGW("mqtt", "link down%s%s, retry in %lu s", err[0] ? ": " : "", err, (unsigned long)(ms / 1000));
}

void startClient() {
  const ServerCfg& v = server();
  buildTopics();
  err[0] = 0;
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = v.host;
  cfg.broker.address.port = v.port;
  cfg.broker.address.transport = v.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
  if (v.tls) {
    if (!caLoaded) { caCache = settings::caPem(); caLoaded = true; }
    if (caCache.length()) cfg.broker.verification.certificate = caCache.c_str();
    else cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.credentials.client_id = clientId;
  if (v.user[0]) cfg.credentials.username = v.user;
  if (v.pass[0]) cfg.credentials.authentication.password = v.pass;
  cfg.session.keepalive = v.keepalive;
  cfg.session.last_will.topic = tState;
  cfg.session.last_will.msg = offlineMsg();
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.disable_auto_reconnect = true;
  cfg.network.timeout_ms = MQTT_NETWORK_TIMEOUT_MS;
  cfg.buffer.size = MQTT_RX_BUFFER;      // پیام قواعد ممکن است چند کیلوبایت باشد
  cfg.buffer.out_size = MQTT_TX_BUFFER;
  cfg.task.stack_size = MQTT_TASK_STACK;
  client = esp_mqtt_client_init(&cfg);
  if (!client) {
    strlcpy(err, "out of memory", sizeof err);
    scheduleRetry(false);
    return;
  }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
  if (esp_mqtt_client_start(client) != ESP_OK) {
    strlcpy(err, "start failed", sizeof err);
    scheduleRetry(false);
    return;
  }
  state = CONNECTING;
  startedAt = millis();
  LOGI("mqtt", "connecting to %s (%s:%u%s)", v.name, v.host, v.port, v.tls ? ", TLS" : "");
}
}  // namespace

void begin() {
  srv = nextServer(-1);
  if (srv < 0) {
    LOGE("mqtt", "no server is enabled");
    return;
  }
  attempt = 0;
  state = WAIT;
  nextAt = millis() + 500;
}

void stop() {
  if (client && state == UP) {
    esp_mqtt_client_publish(client, tState, offlineMsg(), 0, 1, true);
    delay(100);
  }
  teardown();
  state = IDLE;
}

void loop() {
  uint32_t now = millis();
  if (state == IDLE) return;
  if (!net::up()) {
    if (state == CONNECTING || state == UP) {
      teardown();
      if (!downAt) downAt = now ? now : 1;
      state = WAIT;
      nextAt = now;      // as soon as Wi-Fi is back
      if (testIdx >= 0) { testText = "failed: Wi-Fi not connected"; testIdx = -1; state = IDLE; }
    }
    return;
  }
  switch (state) {
    case WAIT:
      if ((int32_t)(now - nextAt) >= 0 && srv >= 0) {
        // TLS تاریخ گواهی را می‌سنجد: تا آماده شدن ساعت (NTP) صبر کن (حداکثر MQTT_TLS_CLOCK_WAIT_MS، بعد باز هم تلاش کن).
        if (server().tls && !net::timeValid()) {
          if (!timeWaitStart) {
            timeWaitStart = now ? now : 1;
            LOGI("mqtt", "waiting for the clock (TLS needs it)");
          }
          if (now - timeWaitStart < MQTT_TLS_CLOCK_WAIT_MS) break;
        }
        timeWaitStart = 0;
        startClient();
      }
      break;
    case CONNECTING:
      if (evConnected) {
        evConnected = false;
        state = UP;
        upAt = now;
        downAt = 0;
        LOGI("mqtt", "connected to %s", server().name);
        if (testIdx >= 0) {
          testText = "ok";
          testIdx = -1;
          teardown();
          state = IDLE;
          return;
        }
        publish(tState, onlineMsg().c_str(), 1, true);
        sendTelemetry();
        lastChanges = relays::changes();
        if (!generic) {
          hubapi::reportPendingOta();
          hubapi::fetchAll();            // local key and rules after every start / reconnect
          local::flushLog();
        }
      } else if (evDown || now - startedAt > MQTT_CONNECT_TIMEOUT_MS) {
        if (!evDown) strlcpy(err, "timeout", sizeof err);
        scheduleRetry(false);
      }
      break;
    case UP: {
      if (evDown) {
        downAt = now ? now : 1;
        scheduleRetry(now - upAt >= MQTT_STABLE_MS);
        break;
      }
      uint32_t ch = relays::changes();
      bool changed = ch != lastChanges;
      if ((changed && now - lastTelemetry >= MQTT_TELEMETRY_MIN_GAP_MS) ||
          now - lastTelemetry >= settings::cur.telemetryS * 1000UL) {
        lastChanges = ch;
        sendTelemetry();
      }
      break;
    }
    default:
      break;
  }
}

bool configured() { return srv >= 0; }
bool connected() { return state == UP; }
int serverIndex() { return state == UP ? srv : -1; }
const char* lastError() { return err; }
uint32_t downSinceMs() { return state == UP ? 0 : downAt; }
uint32_t rxCount() { return rx; }
uint32_t txCount() { return tx; }

void pairing() {
  if (state != UP || generic) {
    LOGW("mqtt", "pairing needs a connected platform server");
    return;
  }
  publish(tPairing, "{}", 1, false);
  LOGI("mqtt", "pairing request sent");
}

void test(uint8_t idx) {
  if (idx >= MAX_SERVERS) return;
  teardown();
  testIdx = idx;
  srv = idx;
  testText = "testing";
  caLoaded = false;
  if (!net::up()) { testText = "failed: Wi-Fi not connected"; testIdx = -1; state = IDLE; return; }
  state = WAIT;
  nextAt = millis();
}

String testResult() { return testText; }

// نتیجهٔ فرمان رله که حلقهٔ رله‌ها گزارش می‌دهد («done» یا «failed»)؛ برای فرمان‌های تکراری هم حفظ می‌شود.
void ackResult(const char* id, bool ok, uint8_t relay, bool value, const char* error) {
  remember(id, ok ? 1 : 2, relay, value);
  sendAck(id, ok ? "done" : "failed", relay, value, error);
}

// پیام d/<id>/<kind> با QoS 1 (مثل ota/status و local_log). فقط به سرور پلتفرم و وقتی وصل هستیم.
bool publishKind(const char* kind, const String& json) {
  if (state != UP || generic || !client) return false;
  char topic[160];
  snprintf(topic, sizeof topic, "%s%s", tBase, kind);
  bool ok = esp_mqtt_client_publish(client, topic, json.c_str(), 0, 1, false) >= 0;
  if (ok) leds::data();
  return ok;
}
}  // namespace cloud
