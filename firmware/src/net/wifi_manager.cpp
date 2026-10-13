#include "wifi_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <vector>

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
constexpr uint32_t AP_OFF_DELAY_NO_CLIENTS_MS = 3000;

enum class StaState : uint8_t { Idle, Connecting, Connected, Failed };

struct Network {
  String ssid;
  int32_t rssi;
  int32_t channel;
  bool secure;
};

DNSServer g_dns;
SemaphoreHandle_t g_mutex = xSemaphoreCreateMutex();

// Zustand (nur loop() schreibt; Strings/Listen unter g_mutex)
std::atomic<bool> g_ap{false};
std::atomic<StaState> g_sta{StaState::Idle};
std::atomic<int> g_last_reason{0};
std::atomic<int> g_disconnect_reason{0};  // letzter Trennungsgrund (für das Ereignisprotokoll)
std::atomic<bool> g_bt_active{false};     // Bluetooth an -> WLAN braucht Modem-Sleep

// Energiesparmodus: ohne Bluetooth aus (geringste Latenz für Steuerung und Stream).
// Laufen WLAN und Bluetooth gleichzeitig, teilt der ESP32 die gemeinsame Antenne
// nur auf, wenn das WLAN zwischen den Beacons schlafen darf (Modem-Sleep) – sonst
// bekommt Bluetooth kaum Funkzeit und das WLAN bricht ständig ab.
wifi_ps_type_t sleepMode() { return g_bt_active ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE; }
uint32_t g_connect_started = 0;
uint32_t g_lost_since = 0;
uint32_t g_last_retry = 0;
uint32_t g_ap_off_at = 0;
bool g_user_connect = false;  // Versuch kam aus der Oberfläche (nicht automatisch)
String g_target_ssid;  // Netz des laufenden/letzten Verbindungsversuchs
String g_fail_reason;

// Anfragen aus den Web-Handlern
std::atomic<bool> g_req_connect{false};
std::atomic<bool> g_req_scan{false};
String g_req_ssid, g_req_pass;

// Netzsuche
bool g_scan_running = false;
uint32_t g_scan_ms = 0;
String g_scan_error;
std::vector<Network> g_networks;

void lock() { xSemaphoreTake(g_mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(g_mutex); }

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

void startMdns() {
  MDNS.end();
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.enableArduino(3232);  // Firmware-Update per PlatformIO/Arduino-IDE (sys/ota.cpp)
  } else {
    log_w("mDNS-Start fehlgeschlagen");
  }
}

const char* reasonText(int reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "Anmeldung abgelehnt – Passwort falsch?";
    case WIFI_REASON_NO_AP_FOUND:
      return "Netz nicht gefunden (Name falsch, zu weit weg oder 5-GHz-Netz?)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      return "Netz nutzt eine nicht unterstützte Verschlüsselung";
    case WIFI_REASON_CONNECTION_FAIL:
      return "Verbindung fehlgeschlagen";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "Funkkontakt zum Router abgerissen (Beacon-Timeout)";
    case WIFI_REASON_ASSOC_LEAVE:
      return "vom ESP32 getrennt";
    default:
      return nullptr;
  }
}

bool isAuthFailure(int reason) {
  return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_EXPIRE ||
         reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY;
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_last_reason = info.wifi_sta_disconnected.reason;
    g_disconnect_reason = info.wifi_sta_disconnected.reason;
  }
}

void startAccessPoint() {
  // AP+STA: Netzsuche und Verbindungsversuche bei laufendem Access Point
  WiFi.setAutoReconnect(false);  // Wiederholungen stören den AP-Kanal -> selbst steuern
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", WiFi.softAPIP());  // Captive Portal: jeder Name -> 192.168.4.1
  g_ap = true;
  g_ap_off_at = 0;
  statusLeds.setState(ledfx::State::Ap);
  Serial.printf("[WiFi] Access Point \"%s\" (Passwort \"%s\") aktiv, IP %s\n", WIFI_AP_SSID,
                WIFI_AP_PASS, WiFi.softAPIP().toString().c_str());
}

void stopAccessPoint() {
  g_dns.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  g_ap = false;
  g_ap_off_at = 0;
  startMdns();
  Serial.println("[WiFi] Access Point beendet");
}

void beginConnect(const String& ssid, const String& pass) {
  WiFi.disconnect(false);
  g_last_reason = 0;
  WiFi.begin(ssid.c_str(), pass.c_str());
  lock();
  g_target_ssid = ssid;
  g_fail_reason = "";
  unlock();
  g_connect_started = millis();
  g_sta = StaState::Connecting;
  Serial.printf("[WiFi] Verbinde mit \"%s\" ...\n", ssid.c_str());
}

void onConnected() {
  g_sta = StaState::Connected;
  g_lost_since = 0;
  Serial.printf("[WiFi] Verbunden mit \"%s\", IP %s, RSSI %d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (g_ap) {
    // Access Point noch eine Weile laufen lassen, damit die Oberfläche die neue
    // Adresse anzeigen kann. Bei einem Versuch aus der Oberfläche immer – das
    // Gerät hat durch den Kanalwechsel evtl. gerade kurz die Verbindung verloren.
    bool linger = g_user_connect || WiFi.softAPgetStationNum() > 0;
    g_ap_off_at = millis() + (linger ? WIFI_AP_LINGER_MS : AP_OFF_DELAY_NO_CLIENTS_MS);
  }
  g_user_connect = false;
  startMdns();
}

void onFailed() {
  int reason = g_last_reason;
  const char* text = reasonText(reason);
  lock();
  g_fail_reason = text ? text : "Zeitüberschreitung beim Verbinden";
  if (!text && reason) g_fail_reason += " (Code " + String(reason) + ")";
  String msg = g_fail_reason;
  unlock();
  g_sta = StaState::Failed;
  WiFi.disconnect(false);  // keine weiteren Versuche, AP-Kanal bleibt stabil
  g_last_retry = millis();
  Serial.printf("[WiFi] Verbindung fehlgeschlagen: %s\n", msg.c_str());
}

void processScan() {
  if (g_req_scan.exchange(false) && !g_scan_running) {
    if (g_sta == StaState::Connecting) {
      lock();
      g_scan_error = "Gerade läuft ein Verbindungsversuch – bitte gleich noch einmal";
      unlock();
    } else {
      int16_t r = WiFi.scanNetworks(true, false);  // asynchron
      g_scan_running = r == WIFI_SCAN_RUNNING || r >= 0;
      lock();
      g_scan_error = g_scan_running ? "" : "Suche konnte nicht gestartet werden";
      unlock();
    }
  }
  if (!g_scan_running) return;

  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  g_scan_running = false;

  std::vector<Network> list;
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // verstecktes Netz
    int32_t rssi = WiFi.RSSI(i);
    bool dup = false;
    for (auto& e : list) {
      if (e.ssid == ssid) {  // gleiches Netz mehrfach (Repeater): stärkstes behalten
        if (rssi > e.rssi) e.rssi = rssi;
        dup = true;
        break;
      }
    }
    if (!dup) list.push_back({ssid, rssi, WiFi.channel(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(list.begin(), list.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });

  lock();
  g_networks = std::move(list);
  g_scan_ms = millis();
  g_scan_error = n < 0 ? "Suche fehlgeschlagen" : "";
  unlock();
}

}  // namespace

void begin() {
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.onEvent(onWifiEvent);

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    statusLeds.setState(ledfx::State::WifiConnect);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(sleepMode());  // ohne Bluetooth: geringere Latenz für Steuerung und Stream
    WiFi.setAutoReconnect(true);
    beginConnect(ssid, pass);
    while (WiFi.status() != WL_CONNECTED && millis() - g_connect_started < WIFI_STA_TIMEOUT_MS &&
           !isAuthFailure(g_last_reason)) {
      statusLeds.update();
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      onConnected();
      return;
    }
    onFailed();
    Serial.println("[WiFi] -> Access Point, Heimnetz wird im Hintergrund weiter versucht");
  }
  WiFi.setSleep(sleepMode());
  startAccessPoint();
  startMdns();
}

void loop() {
  uint32_t now = millis();

  // Neue Zugangsdaten aus der Oberfläche: sofort verbinden
  if (g_req_connect.exchange(false)) {
    lock();
    String ssid = g_req_ssid, pass = g_req_pass;
    unlock();
    g_user_connect = true;
    beginConnect(ssid, pass);
  }

  wl_status_t st = WiFi.status();
  switch (g_sta.load()) {
    case StaState::Connecting:
      if (st == WL_CONNECTED) {
        onConnected();
      } else if (now - g_connect_started > WIFI_CONNECT_TIMEOUT_MS ||
                 (isAuthFailure(g_last_reason) && now - g_connect_started > 2000)) {
        onFailed();
      }
      break;
    case StaState::Connected:
      if (st != WL_CONNECTED) {
        // Verbindung verloren: der WLAN-Stack versucht es selbst erneut. Bleibt
        // das Netz weg, startet der Access Point, damit der Roboter erreichbar bleibt.
        if (g_lost_since == 0) {
          g_lost_since = now ? now : 1;
          Serial.println("[WiFi] Verbindung verloren");
        } else if (!g_ap && now - g_lost_since > WIFI_STA_LOST_FALLBACK_MS) {
          g_sta = StaState::Failed;
          lock();
          g_fail_reason = "Verbindung zum Heimnetz verloren";
          unlock();
          g_last_retry = now;
          startAccessPoint();
        }
      } else {
        g_lost_since = 0;
      }
      break;
    case StaState::Failed:
      // Im Access-Point-Modus regelmäßig erneut versuchen – aber nur, solange
      // niemand mit dem AP verbunden ist (ein Versuch wechselt den Kanal).
      if (g_ap && now - g_last_retry > WIFI_RETRY_PERIOD_MS && WiFi.softAPgetStationNum() == 0) {
        String ssid, pass;
        if (loadCredentials(ssid, pass)) {
          g_user_connect = false;
          beginConnect(ssid, pass);
        } else {
          g_last_retry = now;
        }
      }
      break;
    case StaState::Idle:
      break;
  }

  // Access Point nach erfolgreicher Verbindung abschalten
  if (g_ap && g_ap_off_at && (int32_t)(now - g_ap_off_at) >= 0 && g_sta == StaState::Connected) {
    stopAccessPoint();
  }

  processScan();
}

bool isAccessPoint() { return g_ap; }

void setBluetoothActive(bool on) {
  g_bt_active = on;
  WiFi.setSleep(sleepMode());
}

int lastDisconnectReason() { return g_disconnect_reason; }

const char* disconnectText(int reason) {
  const char* t = reasonText(reason);
  return t ? t : "unbekannter Grund";
}

bool staConnected() { return g_sta == StaState::Connected && WiFi.status() == WL_CONNECTED; }

String ip() { return staConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }

void toJson(JsonObject obj) {
  bool ap = g_ap;
  bool sta = staConnected();
  obj["mode"] = ap ? (sta ? "ap+sta" : "ap") : "sta";
  obj["ip"] = ip();
  obj["host"] = String(WIFI_HOSTNAME) + ".local";
  obj["saved_ssid"] = savedSsid();

  JsonObject a = obj["ap"].to<JsonObject>();
  a["active"] = ap;
  a["ssid"] = WIFI_AP_SSID;
  if (ap) {
    a["ip"] = WiFi.softAPIP().toString();
    a["clients"] = WiFi.softAPgetStationNum();
    if (g_ap_off_at) a["off_in_s"] = (int32_t)(g_ap_off_at - millis()) / 1000;
  }

  JsonObject s = obj["sta"].to<JsonObject>();
  static const char* const names[] = {"idle", "connecting", "connected", "failed"};
  s["state"] = names[(int)g_sta.load()];
  lock();
  s["ssid"] = sta ? WiFi.SSID() : g_target_ssid;
  if (g_sta == StaState::Failed) s["reason"] = g_fail_reason;
  unlock();
  if (sta) {
    s["ip"] = WiFi.localIP().toString();
    s["rssi"] = WiFi.RSSI();
  }
}

void requestScan() { g_req_scan = true; }

void scanToJson(JsonObject obj) {
  lock();
  obj["running"] = g_scan_running || g_req_scan;
  obj["age_s"] = g_scan_ms ? (millis() - g_scan_ms) / 1000 : -1;
  if (!g_scan_error.isEmpty()) obj["error"] = g_scan_error;
  JsonArray arr = obj["networks"].to<JsonArray>();
  for (const auto& n : g_networks) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = n.ssid;
    o["rssi"] = n.rssi;
    o["channel"] = n.channel;
    o["secure"] = n.secure;
  }
  unlock();
}

bool configure(const String& ssid, const String& pass, bool connect_now, String& msg) {
  if (ssid.isEmpty() || ssid.length() > 32) {
    msg = "SSID muss 1–32 Zeichen lang sein";
    return false;
  }
  if (!pass.isEmpty() && (pass.length() < 8 || pass.length() > 64)) {
    msg = "Passwort muss leer (offenes Netz) oder 8–64 Zeichen lang sein";
    return false;
  }
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) {
    msg = "Speichern fehlgeschlagen";
    return false;
  }
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();

  if (connect_now && g_ap) {
    lock();
    g_req_ssid = ssid;
    g_req_pass = pass;
    unlock();
    g_req_connect = true;
    msg = "Gespeichert – verbinde mit „" + ssid + "“ …";
  } else {
    msg = "Gespeichert – wird nach einem Neustart verwendet";
  }
  return true;
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
