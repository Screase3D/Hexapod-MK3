#include "wifi_manager.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "ui/status_leds.h"

// Optional: Standard-Zugangsdaten zur Compile-Zeit (nicht im Git!)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS ""
#endif

namespace wifi {
namespace {
constexpr const char* NVS_NAMESPACE = "wifi";
bool g_ap = false;

void startMdns() {
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
  } else {
    log_w("mDNS-Start fehlgeschlagen");
  }
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  g_ap = true;
  statusLeds.setMode(StatusLeds::Mode::ApMode);
  Serial.printf("[WiFi] Access Point \"%s\" aktiv, IP %s\n", WIFI_AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

bool loadCredentials(String& ssid, String& pass) {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }
  if (ssid.isEmpty()) {
    ssid = WIFI_DEFAULT_SSID;
    pass = WIFI_DEFAULT_PASS;
  }
  return !ssid.isEmpty();
}
}  // namespace

void begin() {
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    statusLeds.setMode(StatusLeds::Mode::WifiConnect);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // geringere Latenz für Steuerung und Stream
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[WiFi] Verbinde mit \"%s\" ", ssid.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
      statusLeds.update();
      delay(100);
      if ((millis() - start) % 1000 < 100) Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      g_ap = false;
      Serial.printf("[WiFi] Verbunden, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      startMdns();
      return;
    }
    Serial.println("[WiFi] Verbindung fehlgeschlagen -> Access Point");
    WiFi.disconnect(true);
  }
  startAccessPoint();
  startMdns();
}

void loop() {
  // Reconnect übernimmt der WiFi-Stack (setAutoReconnect).
}

bool isAccessPoint() { return g_ap; }

bool connected() { return g_ap || WiFi.status() == WL_CONNECTED; }

String ip() { return g_ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }

void toJson(JsonObject obj) {
  obj["mode"] = g_ap ? "ap" : "sta";
  obj["ip"] = ip();
  obj["host"] = String(WIFI_HOSTNAME) + ".local";
  if (g_ap) {
    obj["ssid"] = WIFI_AP_SSID;
    obj["clients"] = WiFi.softAPgetStationNum();
  } else {
    obj["ssid"] = WiFi.SSID();
    obj["rssi"] = WiFi.RSSI();
    obj["connected"] = WiFi.status() == WL_CONNECTED;
  }
}

bool saveCredentials(const String& ssid, const String& pass) {
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 64) return false;
  if (!pass.isEmpty() && pass.length() < 8) return false;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return false;
  bool ok = p.putString("ssid", ssid) > 0;
  p.putString("pass", pass);
  p.end();
  return ok;
}

void clearCredentials() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return;
  p.clear();
  p.end();
}

String savedSsid() {
  String ssid, pass;
  loadCredentials(ssid, pass);
  return ssid;
}

}  // namespace wifi
