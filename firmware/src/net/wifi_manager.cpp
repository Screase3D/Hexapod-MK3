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

// Laufender Verbindungsversuch (nur loop()/begin()). Ein Versuch besteht aus mehreren
// WiFi.begin(): Direkt nach einem Neustart lehnt der Router die erste Anmeldung oft ab
// (AUTH_EXPIRE, Handshake-Timeout – er kennt die alte Sitzung noch); ein zweiter Anlauf
// eine Sekunde später klappt dann. Erst mehrere Ablehnungen in Folge heißen „Passwort falsch“.
constexpr uint32_t RETRY_GAP_MS = 1000;      // Pause zwischen zwei Anläufen
constexpr int AUTH_FAILS_WRONG_PASS = 3;     // so viele Ablehnungen in Folge -> Passwort falsch
constexpr uint32_t AP_RETRY_FIRST_MS = 10000;  // im AP-Modus: erster neuer Versuch, dann doppelt so lang
constexpr uint32_t AP_RETRY_WITH_CLIENTS_MS = 180000;  // mit Geräten am AP seltener (Kanalwechsel stört sie)
std::atomic<int> g_disc_events{0};  // Zähler der Trennungen (Ereignis-Task)
int g_disc_seen = 0;
String g_try_ssid, g_try_pass;
uint32_t g_connect_timeout = 0;
uint32_t g_next_begin = 0;     // nächster Anlauf (0 = keiner geplant)
int g_tries = 0, g_auth_fails = 0;
uint32_t g_ap_retry_ms = AP_RETRY_FIRST_MS;
std::atomic<int> g_last_tries{0};
std::atomic<uint32_t> g_last_connect_ms{0};
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
      return "Anmeldung abgelehnt";
    case WIFI_REASON_AUTH_EXPIRE:
      return "Anmeldung abgelaufen (Router kannte evtl. noch die alte Sitzung)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "Schlüsselaustausch fehlgeschlagen (Handshake-Timeout)";
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
    ++g_disc_events;
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

void beginConnect(const String& ssid, const String& pass, uint32_t timeout_ms) {
  // Wiederholungen steuert stepConnecting() selbst (die automatischen des Arduino-Kerns
  // würden dazwischenfunken); nach dem Verbinden übernimmt wieder der Kern.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  g_last_reason = 0;
  g_disc_seen = g_disc_events;
  g_try_ssid = ssid;
  g_try_pass = pass;
  g_tries = 1;
  g_auth_fails = 0;
  g_next_begin = 0;
  g_connect_timeout = timeout_ms;
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
  g_last_tries = g_tries;
  g_last_connect_ms = millis() - g_connect_started;
  g_ap_retry_ms = AP_RETRY_FIRST_MS;
  g_try_pass = "";
  if (!g_ap) WiFi.setAutoReconnect(true);  // Aussetzer im Betrieb überbrückt der Arduino-Kern
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
  // Ablehnungen nur dann als „Passwort falsch?“ melden, wenn sie sich wiederholt haben
  const char* text = reasonText(reason);
  if (isAuthFailure(reason) && reason != WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) {
    text = g_auth_fails >= AUTH_FAILS_WRONG_PASS ? "Anmeldung mehrfach abgelehnt – Passwort falsch?"
                                                 : "Router hat die Anmeldung nicht angenommen (Signal schwach?)";
  }
  g_try_pass = "";
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

// Laufenden Verbindungsversuch weiterführen: verbunden? neuer Anlauf? aufgeben?
void stepConnecting(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    onConnected();
    return;
  }
  const int ev = g_disc_events;
  if (ev != g_disc_seen) {  // Anlauf gescheitert -> gleich noch einmal
    g_disc_seen = ev;
    // ASSOC_LEAVE = selbst getrennt (WiFi.disconnect vor einem neuen Anlauf): kein Fehlschlag
    if (g_last_reason != WIFI_REASON_ASSOC_LEAVE) {
      g_auth_fails = isAuthFailure(g_last_reason) ? g_auth_fails + 1 : 0;
      g_next_begin = now + RETRY_GAP_MS;
    }
  }
  if (g_auth_fails >= AUTH_FAILS_WRONG_PASS || now - g_connect_started > g_connect_timeout) {
    onFailed();
    return;
  }
  if (g_next_begin && (int32_t)(now - g_next_begin) >= 0) {
    g_next_begin = 0;
    ++g_tries;
    Serial.printf("[WiFi] Anlauf %d (vorher: Code %d)\n", g_tries, g_last_reason.load());
    WiFi.disconnect(false);
    WiFi.begin(g_try_ssid.c_str(), g_try_pass.c_str());
  }
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
    // Alle Kanäle absuchen und den stärksten Zugangspunkt nehmen (Repeater/Mesh mit
    // gleichem Namen); sonst nimmt der ESP den zuerst gefundenen, evtl. weit entfernten
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    beginConnect(ssid, pass, WIFI_STA_TIMEOUT_MS);
    while (g_sta == StaState::Connecting) {
      stepConnecting(millis());
      statusLeds.update();
      delay(50);
    }
    if (g_sta == StaState::Connected) return;
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
    beginConnect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS);
  }

  wl_status_t st = WiFi.status();
  switch (g_sta.load()) {
    case StaState::Connecting:
      stepConnecting(now);
      if (g_sta == StaState::Failed && g_ap && !g_user_connect) {
        // automatischer Versuch im AP-Modus gescheitert: nächster etwas später (bis 60 s)
        g_ap_retry_ms = g_ap_retry_ms * 2 > WIFI_RETRY_PERIOD_MS ? WIFI_RETRY_PERIOD_MS : g_ap_retry_ms * 2;
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
      // Im Access-Point-Modus erneut versuchen: zuerst nach 10 s, dann immer seltener
      // (bis 60 s). Sind Geräte mit dem AP verbunden, nur alle 3 min – ein Versuch
      // wechselt den Funkkanal, die Geräte verlieren kurz die Verbindung.
      if (g_ap) {
        const uint32_t wait = WiFi.softAPgetStationNum() > 0 ? AP_RETRY_WITH_CLIENTS_MS : g_ap_retry_ms;
        if (now - g_last_retry > wait) {
          String ssid, pass;
          if (loadCredentials(ssid, pass)) {
            g_user_connect = false;
            beginConnect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS);
          } else {
            g_last_retry = now;
          }
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

bool bluetoothActive() { return g_bt_active; }

void setBluetoothActive(bool on) {
  g_bt_active = on;
  WiFi.setSleep(sleepMode());
}

int lastDisconnectReason() { return g_disconnect_reason; }

int lastConnectTries() { return g_last_tries; }
uint32_t lastConnectMs() { return g_last_connect_ms; }

String failReason() {
  lock();
  String r = g_fail_reason;
  unlock();
  return r;
}

uint32_t nextRetryS() {
  if (!g_ap || g_sta != StaState::Failed) return 0;
  const uint32_t wait = WiFi.softAPgetStationNum() > 0 ? AP_RETRY_WITH_CLIENTS_MS : g_ap_retry_ms;
  const uint32_t el = millis() - g_last_retry;
  return el >= wait ? 0 : (wait - el + 999) / 1000;
}

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
    s["channel"] = WiFi.channel();
  }
  if (g_last_tries) {  // letzte erfolgreiche Verbindung: Anläufe und Dauer
    s["tries"] = g_last_tries.load();
    s["connect_ms"] = g_last_connect_ms.load();
  }
  if (g_sta == StaState::Connecting) s["try"] = g_tries;
  const uint32_t retry = nextRetryS();
  if (retry) s["retry_in"] = retry;
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
