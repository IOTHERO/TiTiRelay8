#include "hubapi.h"
#include <Preferences.h>
#include <Update.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>
#include "board.h"
#include "cloud.h"
#include "health.h"
#include "local.h"
#include "log.h"
#include "rules.h"
#include "settings.h"

namespace hubapi {
namespace {
constexpr size_t MAX_JSON = 16 * 1024;
enum Job : uint8_t { JOB_FETCH = 1, JOB_OTA = 2 };

QueueHandle_t jobs = nullptr;
volatile bool busy = false;
String ca;

struct Offer {
  char jobId[40];
  char version[24];
  char sha256[65];
  char url[160];
  uint32_t size;
} offer;
SemaphoreHandle_t offerMtx = nullptr;

void otaStatus(const char* jobId, const char* status, const char* version, const char* error) {
  JsonDocument d;
  d["job_id"] = jobId;
  d["status"] = status;
  if (version) d["version"] = version;
  if (error) d["error"] = error;
  cloud::publishKind("ota/status", d);
}

esp_http_client_handle_t open(const String& path, int& status, int64_t& length) {
  String url = String(settings::cur.apiUrl) + path;
  esp_http_client_config_t c = {};
  c.url = url.c_str();
  c.timeout_ms = 15000;
  c.buffer_size = 2048;
  c.keep_alive_enable = false;
  if (url.startsWith("https://")) {
    if (ca.length()) c.cert_pem = ca.c_str();
    else c.crt_bundle_attach = esp_crt_bundle_attach;
  }
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) return nullptr;
  String auth = String("Bearer ") + settings::deviceSecret();
  esp_http_client_set_header(h, "Authorization", auth.c_str());
  esp_http_client_set_header(h, "Accept", "application/json, application/octet-stream");
  if (esp_http_client_open(h, 0) != ESP_OK) {
    esp_http_client_cleanup(h);
    status = -1;
    return nullptr;
  }
  length = esp_http_client_fetch_headers(h);
  status = esp_http_client_get_status_code(h);
  return h;
}

void close(esp_http_client_handle_t h) {
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
}

bool getJson(const String& path, String& out) {
  int status;
  int64_t len;
  esp_http_client_handle_t h = open(path, status, len);
  if (!h) {
    LOGW("hubapi", "GET %s: cannot connect", path.c_str());
    return false;
  }
  bool ok = status == 200;
  if (ok) {
    char buf[512];
    int n;
    while ((n = esp_http_client_read(h, buf, sizeof buf)) > 0) {
      if (out.length() + n > MAX_JSON) { ok = false; break; }
      out.concat(buf, n);
    }
  } else {
    LOGW("hubapi", "GET %s: HTTP %d", path.c_str(), status);
  }
  close(h);
  return ok;
}

void fetch() {
  String base = String("/ingest/v1/devices/") + settings::cur.deviceId;
  String body;
  if (getJson(base + "/config", body)) {
    JsonDocument d;
    if (!deserializeJson(d, body)) local::applyConfig(d["local"]);
  }
  body = "";
  if (getJson(base + "/rules", body)) rules::apply(body.c_str(), body.length());
}

void runOta() {
  Offer o;
  xSemaphoreTake(offerMtx, portMAX_DELAY);
  o = offer;
  xSemaphoreGive(offerMtx);
  LOGI("ota", "firmware %s offered (%lu bytes)", o.version, (unsigned long)o.size);
  otaStatus(o.jobId, "downloading", nullptr, nullptr);
  int status;
  int64_t len;
  esp_http_client_handle_t h = open(o.url, status, len);
  const char* error = nullptr;
  if (!h) error = "cannot connect";
  else if (status != 200) error = "download refused";
  else if (o.size && len > 0 && (uint64_t)len != o.size) error = "size differs from the offer";
  else if (!Update.begin(o.size ? o.size : UPDATE_SIZE_UNKNOWN, U_FLASH)) error = "no room for the image";
  if (!error) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint8_t buf[1024];
    uint32_t total = 0;
    int n;
    while ((n = esp_http_client_read(h, (char*)buf, sizeof buf)) > 0) {
      mbedtls_sha256_update(&sha, buf, n);
      if (Update.write(buf, n) != (size_t)n) { error = "flash write failed"; break; }
      total += n;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (!error && n < 0) error = "download interrupted";
    if (!error && o.size && total != o.size) error = "image incomplete";
    if (!error && strcasecmp(hex, o.sha256) != 0) error = "SHA-256 mismatch";
    if (error) Update.abort();
    else if (!Update.end(true)) error = "image rejected";
  }
  if (h) close(h);
  if (error) {
    LOGE("ota", "update %s failed: %s", o.version, error);
    otaStatus(o.jobId, "failed", nullptr, error);
    return;
  }
  // "installed" is reported by the new firmware once it runs (reportPendingOta).
  Preferences p;
  if (p.begin("ota", false)) {
    p.putString("job", o.jobId);
    p.putString("ver", o.version);
    p.end();
  }
  LOGI("ota", "firmware %s written, restarting", o.version);
  delay(500);
  health::restart("firmware update");
}

void worker(void*) {
  uint8_t job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    if (ca.length() == 0) ca = settings::caPem();
    busy = true;
    if (job == JOB_FETCH) fetch();
    else if (job == JOB_OTA) runOta();
    busy = false;
  }
}
}  // namespace

void begin() {
  if (jobs) return;
  jobs = xQueueCreate(4, sizeof(uint8_t));
  offerMtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(worker, "hubapi", 8192, nullptr, 1, nullptr, 0);
}

bool configured() {
  return settings::cur.apiUrl[0] && settings::cur.deviceId[0] && settings::deviceSecret()[0];
}

void fetchAll() {
  if (!jobs || !configured()) return;
  uint8_t j = JOB_FETCH;
  xQueueSend(jobs, &j, 0);
}

void offerOta(JsonObjectConst o) {
  const char* jobId = o["job_id"] | "";
  const char* version = o["version"] | "";
  if (!*jobId) return;
  if (!settings::cur.otaEnabled) { otaStatus(jobId, "failed", nullptr, "updates disabled on the device"); return; }
  if (!strcmp(version, FW_VERSION)) { otaStatus(jobId, "installed", FW_VERSION, nullptr); return; }
  if (!configured()) { otaStatus(jobId, "failed", nullptr, "no hub address (apiUrl) on the device"); return; }
  if (busy) return;                                  // the hub offers it again later
  xSemaphoreTake(offerMtx, portMAX_DELAY);
  strlcpy(offer.jobId, jobId, sizeof offer.jobId);
  strlcpy(offer.version, version, sizeof offer.version);
  strlcpy(offer.sha256, o["sha256"] | "", sizeof offer.sha256);
  strlcpy(offer.url, o["url"] | "", sizeof offer.url);
  offer.size = o["size"] | 0;
  xSemaphoreGive(offerMtx);
  if (strlen(offer.sha256) != 64 || offer.url[0] != '/') {
    otaStatus(jobId, "failed", nullptr, "offer without sha256 or url");
    return;
  }
  uint8_t j = JOB_OTA;
  xQueueSend(jobs, &j, 0);
}

void reportPendingOta() {
  Preferences p;
  if (!p.begin("ota", false)) return;
  String job = p.getString("job", "");
  String ver = p.getString("ver", "");
  if (job.length()) {
    if (ver == FW_VERSION) otaStatus(job.c_str(), "installed", FW_VERSION, nullptr);
    else otaStatus(job.c_str(), "failed", nullptr, "the new firmware did not start");
    p.clear();
  }
  p.end();
}

bool otaBusy() { return busy; }
}  // namespace hubapi
