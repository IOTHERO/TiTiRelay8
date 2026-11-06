// Serial log: levels, one line per event, repeats of the same message folded together, and a
// small ring buffer the config portal shows live. Safe to call from any task.
#pragma once
#include <Arduino.h>

enum LogLevel : uint8_t { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

namespace logx {
void begin(uint32_t baud);
void setLevel(uint8_t level);
uint8_t level();
void write(LogLevel lvl, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void flushRepeats();   // call from loop(): prints "(repeated N times)" once a quiet window ends

static constexpr uint8_t RING = 48;
static constexpr uint8_t LINE = 112;
struct Entry { uint32_t seq; uint32_t ms; uint8_t lvl; char text[LINE]; };
// Copy entries with seq > after into out (oldest first); returns how many.
uint8_t since(uint32_t after, Entry* out, uint8_t max);
uint32_t lastSeq();
}

#define LOGE(tag, ...) logx::write(LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) logx::write(LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) logx::write(LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) logx::write(LOG_DEBUG, tag, __VA_ARGS__)
