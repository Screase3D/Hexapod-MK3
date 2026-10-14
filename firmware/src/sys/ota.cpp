#include "ota.h"

#include <ArduinoOTA.h>
#include <Update.h>

#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"  // optional: OTA_PASSWORD
#endif
#include "motion/motion.h"
#include "servo/servo_controller.h"
#include "sys/evlog.h"

namespace ota {
namespace {
volatile bool g_active = false;
size_t g_written = 0;
size_t g_size = 0;

// Servos stromlos, nichts mehr bewegen (Flash-Schreiben bremst alles andere)
void prepare() {
  motion::stopWalking();
  servos.setTorque(false);
}
}  // namespace

void begin() {
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.setMdnsEnabled(false);  // mDNS läuft schon (wifi_manager)
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([]() {
    g_active = true;
    prepare();
    servos.setInhibit("Firmware-Update");  // loop() steht während des Uploads; danach setzt main.cpp sie zurück
    EV_INFO("Firmware-Update über PlatformIO gestartet");
  });
  ArduinoOTA.onEnd([]() {
    g_active = false;
    EV_INFO("Firmware-Update erfolgreich – Neustart");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    g_active = false;
    EV_ERROR("Firmware-Update fehlgeschlagen (Fehler %d)", (int)e);
  });
  ArduinoOTA.begin();
}

void loop() { ArduinoOTA.handle(); }

bool active() { return g_active; }

bool webBegin(size_t size, String& err) {
  if (g_active) {
    err = "Es läuft schon ein Update";
    return false;
  }
  prepare();
  if (!Update.begin(size, U_FLASH)) {
    err = String("Update nicht möglich: ") + Update.errorString();
    return false;
  }
  g_active = true;
  g_size = size;
  g_written = 0;
  EV_INFO("Firmware-Update über die Weboberfläche gestartet (%u KB)", (unsigned)(size / 1024));
  return true;
}

bool webWrite(const uint8_t* data, size_t len, String& err) {
  if (g_written == 0 && len > 0 && data[0] != 0xE9) {  // Kennung eines ESP32-App-Abbilds
    err = "Keine ESP32-Firmware (.bin aus .pio/build/hexapod/firmware.bin verwenden)";
    webAbort();
    return false;
  }
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    err = String("Schreiben fehlgeschlagen: ") + Update.errorString();
    webAbort();
    return false;
  }
  g_written += len;
  return true;
}

bool webEnd(String& err) {
  // prüft Länge und Prüfsumme, aktiviert die neue Partition erst danach
  if (!Update.end(true)) {
    err = String("Firmware ungültig: ") + Update.errorString();
    g_active = false;
    EV_ERROR("Firmware-Update fehlgeschlagen: %s", Update.errorString());
    return false;
  }
  g_active = false;
  EV_INFO("Firmware-Update erfolgreich (%u KB) – Neustart", (unsigned)(g_written / 1024));
  return true;
}

void webAbort() {
  if (!g_active) return;
  Update.abort();
  g_active = false;
  EV_WARN("Firmware-Update abgebrochen");
}

}  // namespace ota
