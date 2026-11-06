#include "log.h"
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace logx {
namespace {
constexpr uint32_t REPEAT_WINDOW_MS = 5000;   // same message within this window is folded
const char LVL_CHAR[] = {'E', 'W', 'I', 'D'};

SemaphoreHandle_t mtx = nullptr;
uint8_t curLevel = LOG_INFO;
Entry ring[RING];
uint8_t head = 0;
uint32_t seq = 0;

// Repeat folding: keyed by (tag, format string) so "retry in 12 s" and "retry in 24 s" fold too.
const char* lastTag = nullptr;
const char* lastFmt = nullptr;
uint32_t lastMs = 0;
uint32_t repeats = 0;

void emit(LogLevel lvl, const char* tag, const char* msg) {
  char line[LINE + 24];
  uint32_t ms = millis();
  snprintf(line, sizeof line, "[%6lu.%03lu] %c %-6s %s", (unsigned long)(ms / 1000),
           (unsigned long)(ms % 1000), LVL_CHAR[lvl], tag, msg);
  Serial.println(line);
  Entry& e = ring[head];
  e.seq = ++seq;
  e.ms = ms;
  e.lvl = lvl;
  snprintf(e.text, LINE, "%s: %s", tag, msg);
  head = (head + 1) % RING;
}

void emitRepeats() {
  if (repeats == 0) return;
  char msg[48];
  snprintf(msg, sizeof msg, "(previous message repeated %lu more times)", (unsigned long)repeats);
  emit(LOG_INFO, lastTag ? lastTag : "log", msg);
  repeats = 0;
}
}  // namespace

void begin(uint32_t baud) {
  Serial.begin(baud);
  mtx = xSemaphoreCreateMutex();
}

void setLevel(uint8_t level) { curLevel = level > LOG_DEBUG ? LOG_DEBUG : level; }
uint8_t level() { return curLevel; }

void write(LogLevel lvl, const char* tag, const char* fmt, ...) {
  if (lvl > curLevel || mtx == nullptr) return;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
  uint32_t now = millis();
  if (tag == lastTag && fmt == lastFmt && now - lastMs < REPEAT_WINDOW_MS) {
    repeats++;
    lastMs = now;
    xSemaphoreGive(mtx);
    return;
  }
  emitRepeats();
  char msg[LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  emit(lvl, tag, msg);
  lastTag = tag;
  lastFmt = fmt;
  lastMs = now;
  xSemaphoreGive(mtx);
}

void flushRepeats() {
  if (repeats == 0 || millis() - lastMs < REPEAT_WINDOW_MS) return;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
  emitRepeats();
  lastFmt = nullptr;
  xSemaphoreGive(mtx);
}

uint8_t since(uint32_t after, Entry* out, uint8_t max) {
  uint8_t n = 0;
  if (mtx == nullptr || xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  for (uint8_t i = 0; i < RING && n < max; i++) {
    const Entry& e = ring[(head + i) % RING];
    if (e.seq > after) out[n++] = e;
  }
  xSemaphoreGive(mtx);
  return n;
}

uint32_t lastSeq() { return seq; }
}  // namespace logx
