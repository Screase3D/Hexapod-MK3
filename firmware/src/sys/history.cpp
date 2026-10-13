#include "history.h"

#include <Arduino.h>
#include <WiFi.h>

#include "net/wifi_manager.h"
#include "sensors/battery.h"

namespace history {
namespace {
Ring<SAMPLES> g_ring;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_last_ms = 0;
}  // namespace

void loop() {
  const uint32_t now = millis();
  if (g_last_ms && now - g_last_ms < PERIOD_MS) return;
  g_last_ms = now ? now : 1;
  Sample s;
  const float v = battery.voltage();
  s.bat_mv = v > 0.5f ? (uint16_t)lroundf(v * 1000.0f) : 0;
  s.rssi = wifi::staConnected() ? (int8_t)constrain(WiFi.RSSI(), -127, -1) : 0;
  const float t = temperatureRead();
  s.temp_c = isfinite(t) && t > -40.0f && t < 125.0f ? (int8_t)lroundf(t) : -128;
  s.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  portENTER_CRITICAL(&g_mux);
  g_ring.push(s);
  portEXIT_CRITICAL(&g_mux);
}

void toJson(JsonObject obj) {
  static Ring<SAMPLES> copy;  // Kopie außerhalb des kritischen Abschnitts serialisieren
  portENTER_CRITICAL(&g_mux);
  copy = g_ring;
  const uint32_t last = g_last_ms;
  portEXIT_CRITICAL(&g_mux);
  obj["period_s"] = PERIOD_MS / 1000;
  obj["age_ms"] = last ? millis() - last : 0;  // seit dem neuesten Wert
  JsonArray bat = obj["bat"].to<JsonArray>();
  JsonArray rssi = obj["rssi"].to<JsonArray>();
  JsonArray temp = obj["temp"].to<JsonArray>();
  JsonArray heap = obj["heap"].to<JsonArray>();
  for (int i = 0; i < copy.count(); ++i) {
    const Sample& s = copy.at(i);
    if (s.bat_mv) bat.add(s.bat_mv / 1000.0f);
    else bat.add(nullptr);
    if (s.rssi) rssi.add(s.rssi);
    else rssi.add(nullptr);
    if (s.temp_c != -128) temp.add(s.temp_c);
    else temp.add(nullptr);
    heap.add(s.heap_kb);
  }
}

}  // namespace history
