#include "pad_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
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
const NimBLEUUID UUID_HID("1812");
const NimBLEUUID UUID_BATTERY("180f");

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
// --- geschützt durch g_mux (keine Speicheranforderung im kritischen Abschnitt!) ---
State g_state = State::Off;
bool g_enabled = false;
bool g_req_pair = false, g_req_forget = false;
uint32_t g_pair_until = 0;
pad::State g_pad;
uint32_t g_pad_ms = 0;
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

// --- nur im BLE-Task ---
NimBLEClient* g_client = nullptr;
NimBLERemoteCharacteristic* g_out = nullptr;  // Ausgabebericht (Vibration)
bool g_inited = false;
bool g_scan_fast = false;

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
    portENTER_CRITICAL(&g_mux);
    if (!g_found) {  // der Task holt Adresse und Namen gemeinsam ab
      g_found_addr = a;
      memcpy(g_found_name, name, sizeof(name));
      g_found = true;
    }
    portEXIT_CRITICAL(&g_mux);
  }
} g_scan_cb;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& info) override { NimBLEDevice::injectPassKey(info, 0); }
  void onConfirmPasskey(NimBLEConnInfo& info, uint32_t) override { NimBLEDevice::injectConfirmPasskey(info, true); }
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
    // Abbruch nach 1 s ohne Funkkontakt erkennen (Standard 4 s):
    // der Controller meldet nur Änderungen, ein gehaltener Stick gilt bis dahin weiter
    // 15..30 ms lassen dem WLAN mehr Funkzeit als die üblichen 7,5 ms und reichen für Sticks
    g_client->setConnectionParams(12, 24, 0, 100);
  }
  if (!g_client->connect(a)) {
    EV_WARN("Controller: Verbindung fehlgeschlagen (Fehler %d)", g_client->getLastError());
    if (pairing) NimBLEDevice::deleteBond(a);
    return false;
  }
  if (!g_client->secureConnection()) EV_WARN("Controller: Verschlüsselung/Kopplung fehlgeschlagen");
  bool report = false;
  g_out = nullptr;
  for (NimBLERemoteService* svc : g_client->getServices(true)) {
    const bool hid = svc->getUUID() == UUID_HID;
    if (!hid && svc->getUUID() != UUID_BATTERY) continue;
    for (NimBLERemoteCharacteristic* ch : svc->getCharacteristics(true)) {
      if (ch->canRead()) {  // Lesen löst bei Bedarf die Verschlüsselung aus
        NimBLEAttValue v = ch->readValue();
        if (!hid && v.size() >= 1) {
          portENTER_CRITICAL(&g_mux);
          g_battery = v[0];
          portEXIT_CRITICAL(&g_mux);
        }
      }
      if (ch->canNotify() && ch->subscribe(true, onReport, true) && hid) report = true;
      if (hid && !g_out && (ch->canWrite() || ch->canWriteNoResponse())) g_out = ch;
    }
  }
  if (!report || !g_client->isConnected()) {
    EV_WARN("Controller: keine Eingabedaten – Firmware des Controllers aktuell (BLE)?");
    disconnect();
    if (pairing) NimBLEDevice::deleteBond(a);
    return false;
  }
  const std::string addr = a.toString();
  portENTER_CRITICAL(&g_mux);
  memcpy(g_name, g_found_name, sizeof(g_name));
  copyStr(g_addr, sizeof(g_addr), addr.c_str());
  if (pairing) g_new_pairing = true;
  g_pad = pad::State();
  g_pad_ms = millis();
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
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    portENTER_CRITICAL(&g_mux);
    const bool enabled = g_enabled;
    const bool req_pair = g_req_pair, req_forget = g_req_forget;
    g_req_pair = g_req_forget = false;
    const bool found = g_found;
    const NimBLEAddress found_addr = g_found_addr;
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
      NimBLEDevice::setSecurityAuth(true, false, false);  // koppeln (Bond), ohne PIN
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
          setState(State::Searching);  // Controller aus oder außer Reichweite: wieder suchen
        } else if (rumble_ms && g_out) {
          // Vibration: Hauptmotoren (Bits 0, 1), Stärke 0..100, Dauer in 10 ms
          const uint8_t p = rumble_strength > 100 ? 100 : rumble_strength;
          uint8_t rep[8] = {0x03, 0, 0, p, p, (uint8_t)(rumble_ms / 10 > 255 ? 255 : rumble_ms / 10), 0, 0};
          g_out->writeValue(rep, sizeof(rep), false);
        }
        break;
      case State::Pairing:
        if (found) {
          EV_INFO("Controller gefunden (%s) – verbinde", found_addr.toString().c_str());
          if (!connectTo(found_addr, true)) setState(State::Pairing);  // weiter versuchen
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
        } else {
          startScan(true);
        }
        break;
      case State::Searching:
        if (found) {
          if (!connectTo(found_addr, false)) setState(State::Searching);
        } else {
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

bool latest(pad::State& out) {
  // Der Controller meldet nur Änderungen: gehaltene Sticks bleiben gültig, solange
  // die Verbindung steht (ein Abbruch wird über das BLE-Timeout erkannt)
  portENTER_CRITICAL(&g_mux);
  const bool ok = g_state == State::Connected;
  if (ok) out = g_pad;
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
