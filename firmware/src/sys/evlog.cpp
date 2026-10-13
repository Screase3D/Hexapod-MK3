#include "evlog.h"

#include <Arduino.h>
#include <esp_system.h>
#include <stdarg.h>

namespace evlog {
namespace {
constexpr int CAPACITY = 48;
constexpr size_t TEXT_LEN = 160;  // Bytes (Umlaute zählen doppelt)

struct Entry {
  uint32_t seq;
  uint32_t ms;
  Level level;
  char text[TEXT_LEN];
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
Entry g_entries[CAPACITY];
uint32_t g_seq = 0;
uint32_t g_warn_seq = 0;

const char* levelName(Level l) {
  switch (l) {
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
    default:           return "info";
  }
}
}  // namespace

void add(Level level, const char* fmt, ...) {
  Entry e;
  e.ms = millis();
  e.level = level;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&g_mux);
  e.seq = ++g_seq;
  if (level != Level::Info) g_warn_seq = e.seq;
  g_entries[(e.seq - 1) % CAPACITY] = e;
  portEXIT_CRITICAL(&g_mux);
  Serial.printf("[%s] %s\n", level == Level::Info ? "Ereignis" : (level == Level::Warn ? "Warnung" : "Fehler"),
                e.text);
}

uint32_t seq() {
  portENTER_CRITICAL(&g_mux);
  uint32_t s = g_seq;
  portEXIT_CRITICAL(&g_mux);
  return s;
}

uint32_t warnSeq() {
  portENTER_CRITICAL(&g_mux);
  uint32_t s = g_warn_seq;
  portEXIT_CRITICAL(&g_mux);
  return s;
}

void toJson(JsonObject obj, uint32_t since) {
  // Kopie außerhalb des kritischen Abschnitts serialisieren (JSON braucht Speicher)
  static Entry copy[CAPACITY];
  portENTER_CRITICAL(&g_mux);
  const uint32_t last = g_seq;
  const uint32_t first = last > CAPACITY ? last - CAPACITY + 1 : 1;
  int n = 0;
  for (uint32_t s = first > since + 1 ? first : since + 1; s <= last; ++s) copy[n++] = g_entries[(s - 1) % CAPACITY];
  portEXIT_CRITICAL(&g_mux);
  obj["seq"] = last;
  obj["uptime_ms"] = millis();
  JsonArray arr = obj["entries"].to<JsonArray>();
  for (int i = 0; i < n; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = copy[i].seq;
    o["ms"] = copy[i].ms;
    o["lvl"] = levelName(copy[i].level);
    o["text"] = copy[i].text;
  }
}

const char* resetReason(bool& warn) {
  warn = true;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   warn = false; return "eingeschaltet";
    case ESP_RST_EXT:       warn = false; return "Reset-Taste";
    case ESP_RST_SW:        warn = false; return "Neustart (Software, z. B. nach Update)";
    case ESP_RST_DEEPSLEEP: warn = false; return "aus dem Tiefschlaf";
    case ESP_RST_PANIC:     return "Absturz (Panic) – bitte melden";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "Watchdog (Programm hing)";
    case ESP_RST_BROWNOUT:  return "Unterspannung (Brownout) – Akku schwach oder Servos ziehen die 5 V herunter";
    default:                return "unbekannt";
  }
}

}  // namespace evlog
