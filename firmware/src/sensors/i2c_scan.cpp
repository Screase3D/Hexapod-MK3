#include "i2c_scan.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

namespace i2cscan {
namespace {

struct KnownDevice {
  uint8_t addr;
  const char* name;
  bool expected;  // laut Schaltplan verbaut
};

constexpr KnownDevice KNOWN[] = {
    {PCA1_ADDR, "PCA9685 #1 (Beine rechts)", true},
    {PCA2_ADDR, "PCA9685 #2 (Beine links)", true},
    {MPU6050_ADDR, "MPU6050", true},
    {0x69, "MPU6050 mit AD0 = high (erwartet: AD0 an GND → 0x68)", false},
    {0x70, "PCA9685 All-Call (antwortet bis zur Initialisierung)", false},
};

constexpr uint8_t FIRST_ADDR = 0x08;
constexpr uint8_t LAST_ADDR = 0x77;

SemaphoreHandle_t g_mutex = xSemaphoreCreateMutex();
uint8_t g_found[LAST_ADDR - FIRST_ADDR + 1];
int g_found_count = 0;
uint32_t g_scan_ms = 0;  // millis() beim Scan, 0 = noch nie
uint32_t g_duration_ms = 0;
std::atomic<uint32_t> g_requested{0};
std::atomic<uint32_t> g_completed{0};

const KnownDevice* lookup(uint8_t addr) {
  for (const auto& d : KNOWN) {
    if (d.addr == addr) return &d;
  }
  return nullptr;
}

void runScan(TwoWire& wire) {
  uint8_t found[sizeof(g_found)];
  int count = 0;
  uint32_t start = millis();
  for (uint8_t addr = FIRST_ADDR; addr <= LAST_ADDR; ++addr) {
    wire.beginTransmission(addr);
    if (wire.endTransmission() == 0) found[count++] = addr;
  }
  uint32_t duration = millis() - start;

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  memcpy(g_found, found, count);
  g_found_count = count;
  g_scan_ms = millis();
  g_duration_ms = duration;
  xSemaphoreGive(g_mutex);
}

}  // namespace

void scanNow(TwoWire& wire) {
  runScan(wire);
  g_completed = g_requested.load();

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  Serial.printf("[I2C]   %d Gerät(e) an SDA=%d/SCL=%d:", g_found_count, PIN_I2C_SDA, PIN_I2C_SCL);
  for (int i = 0; i < g_found_count; ++i) Serial.printf(" 0x%02X", g_found[i]);
  Serial.println();
  for (const auto& d : KNOWN) {
    if (!d.expected) continue;
    bool present = memchr(g_found, d.addr, g_found_count) != nullptr;
    if (!present) Serial.printf("[I2C]   FEHLT: 0x%02X %s\n", d.addr, d.name);
  }
  xSemaphoreGive(g_mutex);
}

uint32_t request() { return ++g_requested; }

void process(TwoWire& wire) {
  uint32_t req = g_requested.load();
  if (req == g_completed.load()) return;
  runScan(wire);
  g_completed = req;
}

uint32_t completed() { return g_completed.load(); }

void toJson(JsonObject obj) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  obj["sda"] = PIN_I2C_SDA;
  obj["scl"] = PIN_I2C_SCL;
  obj["freq_khz"] = I2C_FREQ_HZ / 1000;
  obj["scanned"] = g_scan_ms != 0;
  obj["age_s"] = g_scan_ms ? (millis() - g_scan_ms) / 1000 : 0;
  obj["duration_ms"] = g_duration_ms;

  // Gefundene Geräte (bekannte mit Namen)
  JsonArray devices = obj["devices"].to<JsonArray>();
  for (int i = 0; i < g_found_count; ++i) {
    JsonObject d = devices.add<JsonObject>();
    d["addr"] = g_found[i];
    const KnownDevice* k = lookup(g_found[i]);
    d["name"] = k ? k->name : "unbekanntes Gerät";
    d["expected"] = k ? k->expected : false;
  }
  // Erwartete, aber nicht gefundene Geräte
  JsonArray missing = obj["missing"].to<JsonArray>();
  if (g_scan_ms) {
    for (const auto& k : KNOWN) {
      if (!k.expected || memchr(g_found, k.addr, g_found_count)) continue;
      JsonObject d = missing.add<JsonObject>();
      d["addr"] = k.addr;
      d["name"] = k.name;
    }
  }
  xSemaphoreGive(g_mutex);
}

}  // namespace i2cscan
