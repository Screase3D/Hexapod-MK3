#include "pad_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include "net/wifi_manager.h"
#include "sys/evlog.h"
#include <string.h>

namespace padble {
namespace {
constexpr uint16_t GAMEPAD_APPEARANCE = 964;  // HID-Gamepad
constexpr uint32_t PAIRING_MS = 60000;
// Suchen: beim Koppeln ein Drittel der Funkzeit, sonst 10 % (das WLAN teilt sich die Antenne)
constexpr uint16_t SCAN_PAIR_INTERVAL_MS = 90, SCAN_PAIR_WINDOW_MS = 30;
constexpr uint16_t SCAN_IDLE_INTERVAL_MS = 300, SCAN_IDLE_WINDOW_MS = 30;
constexpr uint32_t TASK_PERIOD_MS = 20;
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t ENCRYPT_WAIT_MS = 4000;
// Verbindungsintervall in 1,25-ms-Schritten: 15..30 ms. Der Controller wünscht sich 7,5 ms –
// das wird abgelehnt (Bluetooth belegte die gemeinsame Antenne dann sehr oft). Längere
// Intervalle (30..50 ms) rissen dagegen schon beim Koppeln ständig ab (Funk-Timeout).
constexpr uint16_t CONN_ITVL_MIN = 12, CONN_ITVL_MAX = 24;
// Abbruch nach 2 s ohne Funkkontakt erkennen (Standard 4 s): der Controller meldet nur
// Änderungen, ein gehaltener Stick gilt bis dahin weiter. 1 s war neben dem WLAN zu knapp.
constexpr uint16_t CONN_SUPERVISION_10MS = 200;
// Verbindungsaufbau: Suche nach dem Controller mit 50 % statt 100 % der Funkzeit
constexpr uint16_t CONN_SCAN_ITVL = 32, CONN_SCAN_WINDOW = 16;
// Nach Fehlversuchen kurz Pause (WLAN entlasten), wachsend bis 8 s
constexpr uint32_t RETRY_BASE_MS = 1000, RETRY_MAX_MS = 8000;
constexpr int8_t TX_POWER_DBM = 9;
// Trennt der Controller so kurz nach einem abgelehnten Intervall, bekommt er künftig seinen Wunsch
constexpr uint32_t REJECT_DROP_MS = 15000;
const NimBLEUUID UUID_HID("1812");
const NimBLEUUID UUID_BATTERY("180f");
const NimBLEUUID UUID_REPORT("2a4d");         // HID-Bericht (Eingabe bzw. Ausgabe)
const NimBLEUUID UUID_BATTERY_LEVEL("2a19");
const NimBLEUUID UUID_REPORT_REF("2908");     // Beschreibung eines Berichts: ID, Typ
constexpr uint8_t REPORT_TYPE_OUTPUT = 2;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
// --- geschützt durch g_mux (keine Speicheranforderung im kritischen Abschnitt!) ---
State g_state = State::Off;
bool g_enabled = false;
bool g_req_pair = false, g_req_forget = false;
uint32_t g_pair_until = 0;
pad::State g_pad;
uint32_t g_pad_ms = 0;
uint32_t g_reports = 0;  // Eingabeberichte seit dem Verbinden
int g_battery = -1;
char g_name[32] = "";
char g_addr[18] = "";
bool g_new_pairing = false;
uint8_t g_rumble_strength = 0;
uint16_t g_rumble_ms = 0;
uint32_t g_seen = 0, g_seen_xbox = 0;  // Kopplung: empfangene Meldungen / davon Microsoft
bool g_found = false;         // Suche hat den Controller gefunden (Adresse in g_found_addr)
char g_found_name[32] = "";
NimBLEAddress g_found_addr;   // nur kopieren (keine Speicheranforderung)
int8_t g_found_rssi = 0;

// --- aus den NimBLE-Rückrufen ---
std::atomic<int> g_disc_reason{0};          // letzter Trennungsgrund (NimBLE-Code)
std::atomic<bool> g_upd_logged{false};      // Wunsch nach anderem Intervall schon gemeldet
std::atomic<uint32_t> g_rejected_ms{0};     // Zeitpunkt der letzten Ablehnung (0 = keine)
std::atomic<bool> g_allow_fast{false};      // Rückfall: Controller verträgt keine Ablehnung

// --- nur im BLE-Task ---
NimBLEClient* g_client = nullptr;
NimBLERemoteCharacteristic* g_out = nullptr;  // Ausgabebericht (Vibration)
bool g_inited = false;
bool g_scan_fast = false;

// Ausgabebericht? (Report Reference: Typ 2). Ohne Beschreibung: beschreibbar und ohne Benachrichtigung
// Klartext zu NimBLE-/HCI-Fehlercodes (die Bibliothek liefert hier keine Texte)
const char* bleErrText(int rc) {
  switch (rc) {
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):      return "Funkkontakt abgerissen (Timeout)";
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM): return "vom Controller beendet";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL):    return "vom Roboter beendet";
    case BLE_HS_HCI_ERR(BLE_ERR_LMP_LL_RSP_TMO):     return "Controller antwortet nicht";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT): return "Verbindungsaufbau gescheitert";
    case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING):     return "Schlüssel passt nicht (neu koppeln)";
    case BLE_HS_ETIMEOUT:                            return "Zeitüberschreitung";
    case BLE_HS_ENOTCONN:                            return "nicht verbunden";
    default:                                         return "";
  }
}

// Verbindung weg: kurz nach einer Ablehnung? Dann verträgt der Controller sie nicht
void checkRejectedDrop() {
  const uint32_t t = g_rejected_ms.exchange(0);
  if (!t || g_allow_fast || millis() - t > REJECT_DROP_MS) return;
  g_allow_fast = true;
  EV_WARN("Controller trennt nach abgelehntem Intervall – bekommt es ab jetzt (WLAN wird langsamer)");
}

bool isOutputReport(NimBLERemoteCharacteristic* ch) {
  if (ch->canNotify() || !(ch->canWrite() || ch->canWriteNoResponse())) return false;
  NimBLERemoteDescriptor* ref = ch->getDescriptor(UUID_REPORT_REF);
  if (!ref) return true;
  const NimBLEAttValue v = ref->readValue();
  return v.size() < 2 || v[1] == REPORT_TYPE_OUTPUT;
}

void setState(State s) {
  portENTER_CRITICAL(&g_mux);
  g_state = s;
  portEXIT_CRITICAL(&g_mux);
}

void copyStr(char* dst, size_t n, const char* src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    // Microsoft-Herstellerkennung 0x0006 in den Herstellerdaten
    const std::string mfr = d->getManufacturerData();
    const bool microsoft = mfr.size() >= 2 && (uint8_t)mfr[0] == 0x06 && (uint8_t)mfr[1] == 0x00;
    const bool gamepad = d->haveAppearance() && d->getAppearance() == GAMEPAD_APPEARANCE;
    const std::string addr = d->getAddress().toString();
    const bool xbox_name = d->haveName() && strstr(d->getName().c_str(), "Xbox") != nullptr;
    char target[18];
    bool pairing;
    portENTER_CRITICAL(&g_mux);
    pairing = g_state == State::Pairing;
    ++g_seen;
    if (microsoft || xbox_name) ++g_seen_xbox;
    memcpy(target, g_addr, sizeof(target));
    const bool already = g_found;
    portEXIT_CRITICAL(&g_mux);
    if (already) return;
    // Koppeln: jeder Xbox-Controller (Microsoft-Gamepad mit HID); sonst nur der gemerkte
    const bool match = pairing ? (gamepad || d->isAdvertisingService(UUID_HID)) && (microsoft || xbox_name)
                               : target[0] && strcasecmp(addr.c_str(), target) == 0;
    if (!match) return;
    char name[32];
    copyStr(name, sizeof(name), d->haveName() ? d->getName().c_str() : "Xbox-Controller");
    const NimBLEAddress a = d->getAddress();
    const int8_t rssi = d->getRSSI();
    portENTER_CRITICAL(&g_mux);
    if (!g_found) {  // der Task holt Adresse und Namen gemeinsam ab
      g_found_addr = a;
      memcpy(g_found_name, name, sizeof(name));
      g_found_rssi = rssi;
      g_found = true;
    }
    portEXIT_CRITICAL(&g_mux);
  }
} g_scan_cb;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& info) override { NimBLEDevice::injectPassKey(info, 0); }
  void onConfirmPasskey(NimBLEConnInfo& info, uint32_t) override { NimBLEDevice::injectConfirmPasskey(info, true); }
  void onDisconnect(NimBLEClient*, int reason) override { g_disc_reason = reason; }
  // Der Controller fragt nach dem Verbinden nach einem kürzeren Intervall: ablehnen, wenn es
  // schneller als 30 ms werden könnte (das WLAN braucht die Funkzeit)
  bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params* p) override {
    const bool ok = g_allow_fast || p->itvl_min >= CONN_ITVL_MIN;
    if (!ok) g_rejected_ms = millis() | 1;
    if (!g_upd_logged.exchange(true)) {
      EV_INFO("Controller wünscht Intervall %.1f–%.1f ms: %s", p->itvl_min * 1.25f, p->itvl_max * 1.25f,
              ok ? "angenommen" : "abgelehnt (WLAN braucht die Funkzeit)");
    }
    return ok;
  }
} g_client_cb;

void onReport(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool) {
  if (ch->getRemoteService()->getUUID() == UUID_BATTERY) {
    if (len >= 1) {
      portENTER_CRITICAL(&g_mux);
      g_battery = data[0];
      portEXIT_CRITICAL(&g_mux);
    }
    return;
  }
  pad::State s;
  if (!pad::parseXboxReport(data, len, s)) return;
  portENTER_CRITICAL(&g_mux);
  g_pad = s;
  g_pad_ms = millis();
  ++g_reports;
  portEXIT_CRITICAL(&g_mux);
}

void startScan(bool fast) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    if (fast == g_scan_fast) return;
    scan->stop();
  }
  g_scan_fast = fast;
  scan->setScanCallbacks(&g_scan_cb, false);
  scan->setActiveScan(true);
  scan->setInterval(fast ? SCAN_PAIR_INTERVAL_MS : SCAN_IDLE_INTERVAL_MS);
  scan->setWindow(fast ? SCAN_PAIR_WINDOW_MS : SCAN_IDLE_WINDOW_MS);
  scan->setDuplicateFilter(false);
  scan->start(0, false, true);  // ohne Ende, der Task stoppt
}

void stopScan() {
  if (g_inited && NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
}

void disconnect() {
  g_out = nullptr;
  if (g_client && g_client->isConnected()) g_client->disconnect();
}

// Verbinden, verschlüsseln/koppeln, Berichte abonnieren
bool connectTo(const NimBLEAddress& a, bool pairing) {
  stopScan();
  setState(State::Connecting);
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_client_cb, false);
    g_client->setConnectTimeout(CONNECT_TIMEOUT_MS);
    g_client->setConnectionParams(CONN_ITVL_MIN, CONN_ITVL_MAX, 0, CONN_SUPERVISION_10MS, CONN_SCAN_ITVL,
                                  CONN_SCAN_WINDOW);
  }
  g_upd_logged = false;
  g_rejected_ms = 0;
  if (!g_client->connect(a)) {
    const int err = g_client->getLastError();
    EV_WARN("Controller: Verbindung fehlgeschlagen (%d %s)", err, bleErrText(err));
    if (pairing) NimBLEDevice::deleteBond(a);
    return false;
  }
  if (!g_client->secureConnection()) {
    // Xbox-Controller starten die Kopplung oft selbst; dann kann der eigene Versuch scheitern,
    // während die Verschlüsselung trotzdem zustande kommt -> kurz darauf warten
    const int err = g_client->getLastError();
    const uint32_t t0 = millis();
    while (g_client->isConnected() && !g_client->getConnInfo().isEncrypted() && millis() - t0 < ENCRYPT_WAIT_MS) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!g_client->isConnected() || !g_client->getConnInfo().isEncrypted()) {
      EV_WARN("Controller: Verschlüsselung/Kopplung fehlgeschlagen (%d %s)", err, bleErrText(err));
      disconnect();
      if (pairing) NimBLEDevice::deleteBond(a);
      return false;
    }
  }
  bool report = false;
  g_out = nullptr;
  // vor dem Abonnieren zurücksetzen: ein Bericht, der gleich danach kommt (Zustand beim
  // Verbinden, gehaltene Tasten), bleibt erhalten
  portENTER_CRITICAL(&g_mux);
  g_pad = pad::State();
  g_pad_ms = millis();
  g_reports = 0;
  portEXIT_CRITICAL(&g_mux);
  for (NimBLERemoteService* svc : g_client->getServices(true)) {
    const bool hid = svc->getUUID() == UUID_HID;
    if (!hid && svc->getUUID() != UUID_BATTERY) continue;
    for (NimBLERemoteCharacteristic* ch : svc->getCharacteristics(true)) {
      // nur Berichte (Eingabe/Ausgabe) und Akkustand – nicht Report Map, Control Point, ...
      const NimBLEUUID& uuid = ch->getUUID();
      if (hid ? uuid != UUID_REPORT : uuid != UUID_BATTERY_LEVEL) continue;
      if (!hid && ch->canRead()) {
        NimBLEAttValue v = ch->readValue();
        if (v.size() >= 1) {
          portENTER_CRITICAL(&g_mux);
          g_battery = v[0];
          portEXIT_CRITICAL(&g_mux);
        }
      }
      if (ch->canNotify() && ch->subscribe(true, onReport, true) && hid) report = true;
      if (hid && !g_out && isOutputReport(ch)) g_out = ch;  // Vibration
    }
  }
  if (!report || !g_client->isConnected()) {
    EV_WARN("Controller: keine Eingabedaten – Firmware des Controllers aktuell (BLE)?");
    disconnect();
    if (pairing) NimBLEDevice::deleteBond(a);
    return false;
  }
  {
    const NimBLEConnInfo ci = g_client->getConnInfo();
    EV_INFO("Bluetooth-Verbindung: Intervall %.1f ms, Signal %d dBm, %s, Speicher frei %u KB (größter Block %u KB)",
            ci.getConnInterval() * 1.25f, g_client->getRssi(), ci.isEncrypted() ? "verschlüsselt" : "unverschlüsselt",
            (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
  }
  const std::string addr = a.toString();
  portENTER_CRITICAL(&g_mux);
  memcpy(g_name, g_found_name, sizeof(g_name));
  copyStr(g_addr, sizeof(g_addr), addr.c_str());
  if (pairing) g_new_pairing = true;
  g_state = State::Connected;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

void shutdown() {
  if (!g_inited) return;
  stopScan();
  disconnect();
  NimBLEDevice::deinit(true);  // gibt den Speicher frei, löscht auch den Client
  g_client = nullptr;
  g_out = nullptr;
  g_inited = false;
  wifi::setBluetoothActive(false);  // WLAN wieder ohne Energiesparmodus (geringste Latenz)
}

void task(void*) {
  uint16_t last_itvl = 0;  // zuletzt gesehenes Verbindungsintervall (1,25 ms)
  bool timeout_asked = false;  // eigenes Funk-Timeout in dieser Verbindung schon nachgefordert
  uint8_t itvl_check = 0;
  uint8_t fails = 0;       // Fehlversuche in Folge
  uint32_t retry_at = 0;   // vorher nicht erneut suchen/verbinden
  // Verbindungsversuch mit Pause nach Fehlschlag
  auto attempt = [&](const NimBLEAddress& addr, bool pairing) {
    if (connectTo(addr, pairing)) {
      fails = 0;
      return true;
    }
    checkRejectedDrop();
    if (fails < 8) ++fails;
    uint32_t wait = RETRY_BASE_MS << (fails - 1);
    const uint32_t cap = pairing ? 2000 : RETRY_MAX_MS;
    retry_at = millis() + (wait > cap ? cap : wait);
    return false;
  };
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    portENTER_CRITICAL(&g_mux);
    const bool enabled = g_enabled;
    // Vergessen bei ausgeschaltetem Bluetooth: erst nach dem nächsten Einschalten ausführen
    // (die Kopplung lässt sich nur mit laufendem Stack löschen)
    const bool req_pair = g_req_pair, req_forget = g_req_forget && g_enabled;
    g_req_pair = false;
    if (g_enabled) g_req_forget = false;
    const bool found = g_found;
    const NimBLEAddress found_addr = g_found_addr;
    const int found_rssi = g_found_rssi;
    g_found = false;
    const bool have_addr = g_addr[0] != 0;
    State st = g_state;
    const bool pair_over = st == State::Pairing && (int32_t)(millis() - g_pair_until) >= 0;
    const uint8_t rumble_strength = g_rumble_strength;
    const uint16_t rumble_ms = g_rumble_ms;
    g_rumble_ms = 0;
    portEXIT_CRITICAL(&g_mux);

    if (!enabled) {
      shutdown();
      setState(State::Off);
      continue;
    }
    if (!g_inited) {
      // Erst das WLAN auf Modem-Sleep, sonst bekommt Bluetooth keine Funkzeit und das
      // WLAN bricht im Sekundentakt ab
      wifi::setBluetoothActive(true);
      vTaskDelay(pdMS_TO_TICKS(50));
      NimBLEDevice::init("Hexapod MK3");
      NimBLEDevice::setSecurityAuth(true, false, true);  // koppeln (Bond), ohne PIN, LE Secure Connections
      NimBLEDevice::setPower(TX_POWER_DBM);
      g_inited = true;
      EV_INFO("Bluetooth an (WLAN im Energiesparmodus, freier Speicher %u KB)", (unsigned)(ESP.getFreeHeap() / 1024));
      st = have_addr ? State::Searching : State::Idle;
      setState(st);
    }

    if (req_forget) {
      stopScan();
      disconnect();
      NimBLEDevice::deleteAllBonds();
      portENTER_CRITICAL(&g_mux);
      g_addr[0] = g_name[0] = 0;
      g_battery = -1;
      portEXIT_CRITICAL(&g_mux);
      st = State::Idle;
      setState(st);
    }
    if (req_pair) {
      disconnect();
      fails = 0;
      retry_at = 0;
      portENTER_CRITICAL(&g_mux);
      g_pair_until = millis() + PAIRING_MS;
      g_state = State::Pairing;
      g_seen = g_seen_xbox = 0;
      portEXIT_CRITICAL(&g_mux);
      st = State::Pairing;
    }

    switch (st) {
      case State::Connected:
        if (!g_client || !g_client->isConnected()) {
          g_out = nullptr;
          last_itvl = 0;
          timeout_asked = false;
          checkRejectedDrop();
          setState(State::Searching);  // Controller aus oder außer Reichweite: wieder suchen
          break;
        }
        if (++itvl_check >= 50) {  // etwa jede Sekunde: hat sich das Intervall geändert?
          itvl_check = 0;
          const NimBLEConnInfo ci = g_client->getConnInfo();
          const uint16_t itvl = ci.getConnInterval();
          if (itvl && itvl != last_itvl) {
            if (last_itvl) EV_INFO("Bluetooth-Intervall jetzt %.1f ms", itvl * 1.25f);
            last_itvl = itvl;
          }
          // Hat der Controller längere Funk-Timeouts bzw. Latenz ausgehandelt, würde ein
          // Abbruch erst spät erkannt (ein gehaltener Stick gilt so lange weiter): die eigenen
          // 2 s nachfordern, einmal je Verbindung
          if (itvl && !timeout_asked && (ci.getConnTimeout() > CONN_SUPERVISION_10MS || ci.getConnLatency() > 0)) {
            timeout_asked = true;
            const bool ok = g_client->updateConnParams(itvl, itvl, 0, CONN_SUPERVISION_10MS);
            EV_INFO("Controller: Funk-Timeout %u ms, Latenz %u – fordere %u ms an%s", ci.getConnTimeout() * 10u,
                    ci.getConnLatency(), CONN_SUPERVISION_10MS * 10u, ok ? "" : " (abgelehnt)");
          }
        }
        if (rumble_ms && g_out) {
          // Vibration: Hauptmotoren (Bits 0, 1), Stärke 0..100, Dauer in 10 ms
          const uint8_t p = rumble_strength > 100 ? 100 : rumble_strength;
          uint8_t rep[8] = {0x03, 0, 0, p, p, (uint8_t)(rumble_ms / 10 > 255 ? 255 : rumble_ms / 10), 0, 0};
          g_out->writeValue(rep, sizeof(rep), false);
        }
        break;
      case State::Pairing:
        if (found && !pair_over) {
          EV_INFO("Controller gefunden (%s, Signal %d dBm) – verbinde", found_addr.toString().c_str(), found_rssi);
          if (!attempt(found_addr, true)) {
            stopScan();
            setState(State::Pairing);  // nach der Pause weiter versuchen
          }
        } else if (pair_over) {
          stopScan();
          setState(have_addr ? State::Searching : State::Idle);
          portENTER_CRITICAL(&g_mux);
          const uint32_t seen = g_seen, seen_xbox = g_seen_xbox;
          portEXIT_CRITICAL(&g_mux);
          if (seen == 0) {
            EV_WARN("Kopplung beendet: Bluetooth hat nichts empfangen (Funkproblem?)");
          } else if (seen_xbox == 0) {
            EV_WARN("Kopplung beendet: kein Xbox-Controller gefunden (%u Bluetooth-Meldungen) – Firmware des "
                    "Controllers aktuell? Kopplungstaste halten, bis das Logo schnell blinkt", (unsigned)seen);
          } else {
            EV_WARN("Kopplung beendet: Xbox-Gerät gesehen, aber nicht verbunden (siehe Einträge davor)");
          }
        } else if ((int32_t)(millis() - retry_at) >= 0) {
          startScan(true);
        }
        break;
      case State::Searching:
        if (found) {
          if (fails == 0) EV_INFO("Controller in Reichweite (Signal %d dBm) – verbinde", found_rssi);
          if (!attempt(found_addr, false)) {
            stopScan();
            setState(State::Searching);
          }
        } else if ((int32_t)(millis() - retry_at) >= 0) {
          startScan(false);
        }
        break;
      case State::Connecting:  // Verbindungsversuch abgebrochen
        setState(have_addr ? State::Searching : State::Idle);
        break;
      default:  // Idle: nichts gekoppelt, kein Funk
        stopScan();
        break;
    }
  }
}
}  // namespace

const char* disconnectText() {
  const int rc = g_disc_reason.load();
  if (rc == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM)) return "vom Controller beendet (ausgeschaltet?)";
  if (rc == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO)) return "Funkkontakt abgerissen (außer Reichweite oder Funk überlastet)";
  const char* t = bleErrText(rc);
  return *t ? t : "unbekannter Grund";
}

const char* stateName(State s) {
  switch (s) {
    case State::Idle:       return "idle";
    case State::Searching:  return "searching";
    case State::Pairing:    return "pairing";
    case State::Connecting: return "connecting";
    case State::Connected:  return "connected";
    default:                return "off";
  }
}

void begin(bool enabled, const char* addr) {
  portENTER_CRITICAL(&g_mux);
  g_enabled = enabled;
  copyStr(g_addr, sizeof(g_addr), addr ? addr : "");
  portEXIT_CRITICAL(&g_mux);
  xTaskCreatePinnedToCore(task, "padble", 8192, nullptr, 1, nullptr, 0);
}

void setEnabled(bool on) {
  portENTER_CRITICAL(&g_mux);
  g_enabled = on;
  portEXIT_CRITICAL(&g_mux);
}

void startPairing() {
  portENTER_CRITICAL(&g_mux);
  g_req_pair = true;
  portEXIT_CRITICAL(&g_mux);
}

void forget() {
  portENTER_CRITICAL(&g_mux);
  g_req_forget = true;
  // Adresse sofort vergessen: auch mit Bluetooth aus darf er sich nicht wieder verbinden
  g_addr[0] = g_name[0] = 0;
  g_battery = -1;
  portEXIT_CRITICAL(&g_mux);
}

Status status() {
  Status s;
  portENTER_CRITICAL(&g_mux);
  s.state = g_state;
  memcpy(s.name, g_name, sizeof(s.name));
  memcpy(s.addr, g_addr, sizeof(s.addr));
  s.battery = g_state == State::Connected ? g_battery : -1;
  if (g_state == State::Pairing) {
    const int32_t left = (int32_t)(g_pair_until - millis());
    s.pairing_left_s = left > 0 ? (left + 999) / 1000 : 0;
  }
  portEXIT_CRITICAL(&g_mux);
  return s;
}

bool latest(pad::State& out, uint32_t* reports) {
  // Der Controller meldet nur Änderungen: gehaltene Sticks bleiben gültig, solange
  // die Verbindung steht (ein Abbruch wird über das BLE-Timeout erkannt)
  portENTER_CRITICAL(&g_mux);
  const bool ok = g_state == State::Connected;
  if (ok) out = g_pad;
  if (reports) *reports = g_reports;
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

void rumble(uint8_t strength, uint16_t ms) {
  portENTER_CRITICAL(&g_mux);
  g_rumble_strength = strength;
  g_rumble_ms = ms;
  portEXIT_CRITICAL(&g_mux);
}

bool takePairedAddress(char out[18]) {
  portENTER_CRITICAL(&g_mux);
  const bool n = g_new_pairing;
  if (n) memcpy(out, g_addr, 18);
  g_new_pairing = false;
  portEXIT_CRITICAL(&g_mux);
  return n;
}

}  // namespace padble
