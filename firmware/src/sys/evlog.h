#pragma once
// Ereignisprotokoll: die letzten Ereignisse (Start und Neustartgrund, WLAN, Not-Aus,
// Akku, Controller, Firmware-Update, ...) im RAM, für die Oberfläche (System ->
// Ereignisse) und zusätzlich auf der seriellen Schnittstelle. Thread-sicher.

#include <ArduinoJson.h>
#include <stdint.h>

namespace evlog {

enum class Level : uint8_t { Info, Warn, Error };

void add(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

uint32_t seq();       // Nummer des neuesten Eintrags (0 = keiner)
uint32_t warnSeq();   // Nummer der neuesten Warnung/des neuesten Fehlers (0 = keine)
// Einträge mit Nummer > since (älteste zuerst)
void toJson(JsonObject obj, uint32_t since);

// Grund des letzten Neustarts als Text; warn = ungewöhnlich (Absturz, Unterspannung, ...)
const char* resetReason(bool& warn);

}  // namespace evlog

#define EV_INFO(...) evlog::add(evlog::Level::Info, __VA_ARGS__)
#define EV_WARN(...) evlog::add(evlog::Level::Warn, __VA_ARGS__)
#define EV_ERROR(...) evlog::add(evlog::Level::Error, __VA_ARGS__)
