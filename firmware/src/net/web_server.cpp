#include "web_server.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <errno.h>
#include <esp_http_server.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <atomic>

#include "config.h"
#include "generated/web_assets.h"
#include "net/camera.h"
#include "net/commands.h"
#include "motion/motion.h"
#include "net/wifi_manager.h"
#include "sensors/battery.h"
#include "sensors/i2c_scan.h"
#include "servo/servo_controller.h"
#include "sys/evlog.h"
#include "sys/history.h"
#include "sys/ota.h"
#include "ui/status_leds.h"
#include "input/pad_control.h"

namespace web {
namespace {

constexpr size_t MAX_BODY_LEN = 8192;
constexpr size_t MAX_WS_FRAME_LEN = 2048;

httpd_handle_t g_server = nullptr;
volatile uint32_t g_last_cmd_ms = 0;
volatile int g_ws_clients = 0;
// Telemetrie und Live-Daten, die ausgelassen wurden, weil das WLAN nicht nachkam
std::atomic<uint32_t> g_ws_dropped{0};
std::atomic<int> g_queued[2] = {{0}, {0}};  // noch nicht gesendete Aufträge: [0] Telemetrie, [1] Live

// Clients der Live-Ansicht (nur im HTTP-Server-Task benutzt); g_live_until für loop()
struct LiveSub {
  int fd = -1;
  uint32_t until = 0;
};
LiveSub g_live_subs[4];
volatile uint32_t g_live_until = 0;

bool liveSubscribed(int fd, uint32_t now) {
  for (const auto& s : g_live_subs) {
    if (s.fd == fd && (int32_t)(s.until - now) > 0) return true;
  }
  return false;
}

void liveSubscribe(int fd, bool on) {
  const uint32_t now = millis();
  LiveSub* slot = nullptr;
  for (auto& s : g_live_subs) {
    if (s.fd == fd) slot = &s;
  }
  if (!slot) {
    for (auto& s : g_live_subs) {
      if (s.fd < 0 || (int32_t)(s.until - now) <= 0) slot = &s;
    }
  }
  if (!slot) return;  // mehr als 4 Live-Ansichten gleichzeitig: die übrigen bekommen nichts
  slot->fd = fd;
  slot->until = on ? now + LIVE_HOLD_MS : now;
  uint32_t until = now;
  for (const auto& s : g_live_subs) {
    if (s.fd >= 0 && (int32_t)(s.until - until) > 0) until = s.until;
  }
  g_live_until = until;
}

// --- Hilfsfunktionen ------------------------------------------------------------

esp_err_t sendJson(httpd_req_t* req, const JsonDocument& doc, const char* status = nullptr) {
  String out;
  serializeJson(doc, out);
  if (status) httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), out.length());
}

esp_err_t sendResult(httpd_req_t* req, bool ok, const String& msg, const char* status = nullptr) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (!msg.isEmpty()) doc["msg"] = msg;
  return sendJson(req, doc, status ? status : (ok ? nullptr : "400 Bad Request"));
}

bool readJsonBody(httpd_req_t* req, JsonDocument& doc, String& err) {
  if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) {
    err = "leerer oder zu großer Body";
    return false;
  }
  String body;
  body.reserve(req->content_len);
  char buf[512];
  size_t remaining = req->content_len;
  int timeouts = 0;
  while (remaining > 0) {
    int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    // Kommt der Rest nicht (Handy aus dem WLAN, falsche Länge), nicht ewig warten: der
    // Server-Task bearbeitet sonst nichts anderes mehr, auch keinen Not-Aus
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (r <= 0) {
      err = "Lesefehler";
      return false;
    }
    timeouts = 0;
    body.concat(buf, r);
    remaining -= r;
  }
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    err = String("JSON-Fehler: ") + e.c_str();
    return false;
  }
  return true;
}

// --- Statische Dateien (gzip, aus dem Flash) -----------------------------------

esp_err_t assetHandler(httpd_req_t* req) {
  char path[64];
  size_t n = strcspn(req->uri, "?#");
  if (n >= sizeof(path)) n = sizeof(path) - 1;
  memcpy(path, req->uri, n);
  path[n] = '\0';
  if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

  for (size_t i = 0; i < WEB_ASSETS_COUNT; ++i) {
    const WebAsset& a = WEB_ASSETS[i];
    if (strcmp(a.path, path) != 0) continue;
    httpd_resp_set_type(req, a.mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)a.data, a.len);
  }
  // Captive Portal: Im Access-Point-Modus landen die Verbindungstests der
  // Betriebssysteme (generate_204, hotspot-detect.html, connecttest.txt …) hier.
  // Die Weiterleitung lässt Handy/Laptop die Oberfläche automatisch öffnen.
  if (wifi::isAccessPoint()) {
    static char location[40];
    snprintf(location, sizeof(location), "http://%s/", WiFi.softAPIP().toString().c_str());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
  }
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nicht gefunden");
  return ESP_OK;
}

// --- REST-API -------------------------------------------------------------------

esp_err_t statusHandler(httpd_req_t* req) {
  JsonDocument doc;
  commands::buildStatus(doc);
  return sendJson(req, doc);
}

esp_err_t calibrationGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  servos.calibrationToJson(doc);
  return sendJson(req, doc);
}

esp_err_t calibrationPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (!servos.calibrationFromJson(doc.as<JsonVariantConst>(), err)) return sendResult(req, false, err);
  bool saved = servos.saveCalibration();
  return sendResult(req, saved, saved ? "Kalibrierung importiert und gespeichert"
                                      : "Importiert, aber Speichern fehlgeschlagen",
                    saved ? nullptr : "500 Internal Server Error");
}

esp_err_t cameraGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  camera::toJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t cameraPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (!camera::applyJson(doc.as<JsonVariantConst>(), err)) return sendResult(req, false, err);
  JsonDocument out;
  camera::toJson(out.to<JsonObject>());
  out["ok"] = true;
  return sendJson(req, out);
}

esp_err_t wifiGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  wifi::toJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

// {"ssid","pass","connect":true} speichern (+ im AP-Modus sofort verbinden)
// {"clear":true} gespeicherte Zugangsdaten löschen
esp_err_t wifiPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (doc["clear"] | false) {
    wifi::clearCredentials();
    return sendResult(req, true, "WLAN-Daten gelöscht – nach einem Neustart startet der Access Point");
  }
  String msg;
  bool ok = wifi::configure(doc["ssid"] | "", doc["pass"] | "", doc["connect"] | false, msg);
  return sendResult(req, ok, msg);
}

esp_err_t wifiScanGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  wifi::scanToJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t wifiScanPostHandler(httpd_req_t* req) {
  wifi::requestScan();
  return sendResult(req, true, "Suche gestartet");
}

esp_err_t ledsGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  statusLeds.configToJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t ledsPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (!statusLeds.applyJson(doc, err)) return sendResult(req, false, err);
  JsonDocument out;
  statusLeds.configToJson(out.to<JsonObject>());
  out["ok"] = true;
  return sendJson(req, out);
}

esp_err_t padGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  padctl::configToJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t padPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (!padctl::applyJson(doc, err)) return sendResult(req, false, err);
  JsonDocument out;
  padctl::configToJson(out.to<JsonObject>());
  out["ok"] = true;
  return sendJson(req, out);
}

esp_err_t logHandler(httpd_req_t* req) {
  // ?since=N: nur neuere Einträge
  uint32_t since = 0;
  char query[32], val[12];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
    since = strtoul(val, nullptr, 10);
  }
  JsonDocument doc;
  evlog::toJson(doc.to<JsonObject>(), since);
  return sendJson(req, doc);
}

esp_err_t historyHandler(httpd_req_t* req) {
  JsonDocument doc;
  history::toJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t batteryGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  battery.toJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t batteryPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  bool enabled = doc["enabled"] | battery.enabled();
  bool deep_sleep = doc["deep_sleep"] | battery.deepSleepEnabled();
  battery.configure(enabled, deep_sleep);
  JsonDocument out;
  battery.toJson(out.to<JsonObject>());
  out["ok"] = true;
  out["msg"] = enabled ? (deep_sleep ? "Akkuüberwachung an, Tiefschlaf an"
                                     : "Akkuüberwachung an, Tiefschlaf aus")
                       : "Akkuüberwachung aus";
  return sendJson(req, out);
}

esp_err_t kinematicsHandler(httpd_req_t* req) {
  JsonDocument doc;
  motion::geometryToJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

esp_err_t i2cGetHandler(httpd_req_t* req) {
  JsonDocument doc;
  i2cscan::toJson(doc.to<JsonObject>());
  return sendJson(req, doc);
}

// Scan anfordern und auf das Ergebnis des Regel-Tasks warten (wenige ms)
esp_err_t i2cScanHandler(httpd_req_t* req) {
  uint32_t id = i2cscan::request();
  uint32_t start = millis();
  while ((int32_t)(i2cscan::completed() - id) < 0 && millis() - start < 1000) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  JsonDocument doc;
  i2cscan::toJson(doc.to<JsonObject>());
  bool done = (int32_t)(i2cscan::completed() - id) >= 0;
  doc["ok"] = done;
  if (!done) doc["msg"] = "Scan nicht abgeschlossen (Regel-Task läuft nicht?)";
  return sendJson(req, doc, done ? nullptr : "503 Service Unavailable");
}

// Firmware-Update: Body = firmware.bin (application/octet-stream)
esp_err_t otaHandler(httpd_req_t* req) {
  constexpr size_t CHUNK = 4096;
  String err;
  if (req->content_len < 64 * 1024) return sendResult(req, false, "Datei zu klein für eine Firmware");
  if (!ota::webBegin(req->content_len, err)) return sendResult(req, false, err);
  uint8_t* buf = (uint8_t*)malloc(CHUNK);
  if (!buf) {
    ota::webAbort();
    return sendResult(req, false, "Kein Speicher", "500 Internal Server Error");
  }
  size_t remaining = req->content_len;
  int timeouts = 0;
  bool ok = true;
  while (remaining > 0) {
    int r = httpd_req_recv(req, (char*)buf, remaining < CHUNK ? remaining : CHUNK);
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
    if (r <= 0) {
      err = "Übertragung abgebrochen";
      ota::webAbort();
      ok = false;
      break;
    }
    timeouts = 0;
    if (!ota::webWrite(buf, r, err)) {
      ok = false;
      break;
    }
    remaining -= r;
  }
  free(buf);
  if (ok) ok = ota::webEnd(err);
  if (!ok) return sendResult(req, false, err);
  commands::requestReboot();
  return sendResult(req, true, "Firmware übertragen – startet neu");
}

// Not-Aus auch ohne WebSocket erreichbar
esp_err_t estopHandler(httpd_req_t* req) {
  servos.emergencyStop();
  return sendResult(req, true, "Not-Aus ausgelöst");
}

esp_err_t rebootHandler(httpd_req_t* req) {
  servos.setTorque(false);
  commands::requestReboot();
  return sendResult(req, true, "Neustart...");
}

// --- WebSocket ------------------------------------------------------------------

constexpr uint32_t WS_SEND_DEADLINE_MS = 4000;
int g_ws_errno = 0;  // letzter Sendefehler (nur im HTTP-Server-Task)

// Senden ohne halbe Frames. lwIP gibt bei stockendem WLAN nach dem Sende-Timeout auch
// teilweise Schreibvorgänge zurück; der HTTP-Server wertet die als Erfolg und schickt den
// nächsten Frame mitten in den Rest des vorigen – der Browser bricht die Verbindung dann
// mit einem Protokollfehler ab. Hier: alles senden oder Fehler (Verbindung wird geschlossen).
int wsSend(httpd_handle_t, int fd, const char* buf, size_t len, int flags) {
  const uint32_t start = millis();
  size_t sent = 0;
  while (sent < len) {
    const int r = send(fd, buf + sent, len - sent, flags);
    if (r > 0) {
      sent += r;
      continue;
    }
    const int err = r < 0 ? errno : ENOTCONN;  // 0 Bytes: Verbindung ist zu
    const bool again = err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
    g_ws_errno = err;
    if (!again) return HTTPD_SOCK_ERR_FAIL;
    if (millis() - start >= WS_SEND_DEADLINE_MS) return HTTPD_SOCK_ERR_TIMEOUT;
  }
  return (int)len;
}

// Fehler beim Senden ins Ereignisprotokoll (höchstens alle 10 s). Hat der Browser die
// Verbindung schon geschlossen (Seite neu geladen, Handy gesperrt), ist das kein Fehler.
void logWsSendFailure(const char* what, esp_err_t err) {
  const int e = g_ws_errno;
  g_ws_errno = 0;
  // errno 0: Fehler ohne eigene Sendefunktion (Sitzung schon geschlossen)
  if (e == 0 || e == ECONNRESET || e == EPIPE || e == ENOTCONN || e == ECONNABORTED || e == EBADF) return;
  static uint32_t last = 0;
  static uint32_t suppressed = 0;
  const uint32_t now = millis();
  if (last && now - last < 10000) {
    ++suppressed;
    return;
  }
  const char* why = e == EAGAIN || e == EWOULDBLOCK ? "WLAN hängt" : esp_err_to_name(err);
  if (suppressed) EV_WARN("WebSocket: %s fehlgeschlagen (%s, errno %d) – Verbindung geschlossen (+%u weitere)", what, why, e, (unsigned)suppressed);
  else EV_WARN("WebSocket: %s fehlgeschlagen (%s, errno %d) – Verbindung geschlossen", what, why, e);
  last = now ? now : 1;
  suppressed = 0;
}

esp_err_t wsHandler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {  // Handshake abgeschlossen
    httpd_sess_set_send_override(req->handle, httpd_req_to_sockfd(req), wsSend);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) return ret;
  if (frame.len == 0) return ESP_OK;
  if (frame.len > MAX_WS_FRAME_LEN) return ESP_FAIL;  // Verbindung schließen

  uint8_t* buf = (uint8_t*)malloc(frame.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  frame.payload = buf;
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT) {
    free(buf);
    return ret;
  }
  buf[frame.len] = '\0';
  g_last_cmd_ms = millis();

  JsonDocument in;
  JsonDocument out;
  DeserializationError e = deserializeJson(in, (const char*)buf, frame.len);
  free(buf);
  if (!e && strcmp(in["cmd"] | "", "live") == 0) {
    // Live-Ansicht an/aus für diesen Client (ohne Antwort; die Oberfläche erneuert alle 1,5 s)
    liveSubscribe(httpd_req_to_sockfd(req), in["on"] | true);
    return ESP_OK;
  }
  if (e) {
    out["type"] = "ack";
    out["ok"] = false;
    out["msg"] = String("JSON-Fehler: ") + e.c_str();
  } else {
    commands::handle(in.as<JsonVariantConst>(), out);
  }
  if (out.isNull()) return ESP_OK;

  String reply;
  serializeJson(out, reply);
  httpd_ws_frame_t resp = {};
  resp.type = HTTPD_WS_TYPE_TEXT;
  resp.payload = (uint8_t*)reply.c_str();
  resp.len = reply.length();
  const esp_err_t sent = httpd_ws_send_frame(req, &resp);
  if (sent != ESP_OK) logWsSendFailure("Antwort", sent);  // Fehler -> Server schließt die Verbindung
  return sent;
}

struct BroadcastJob {
  char* data;
  size_t len;
  bool live;  // nur an Clients der Live-Ansicht
};

// Hat der Socket gerade Platz im Sendepuffer? (lwIP meldet "beschreibbar", solange
// mindestens die Hälfte frei ist.) Ohne Platz würde send() den Server-Task blockieren –
// und mit ihm alle eingehenden Befehle, auch die Fahrbefehle.
bool socketWritable(int fd) {
  fd_set w;
  FD_ZERO(&w);
  FD_SET(fd, &w);
  timeval tv = {0, 0};
  return select(fd + 1, nullptr, &w, nullptr, &tv) > 0 && FD_ISSET(fd, &w);
}

// Läuft im Kontext des HTTP-Server-Tasks (httpd_queue_work) -> thread-sicher
void broadcastWork(void* arg) {
  BroadcastJob* job = static_cast<BroadcastJob*>(arg);
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  size_t count = CONFIG_LWIP_MAX_SOCKETS;
  int ws = 0;
  const uint32_t now = millis();
  if (httpd_get_client_list(g_server, &count, fds) == ESP_OK) {
    for (size_t i = 0; i < count; ++i) {
      if (httpd_ws_get_fd_info(g_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
      ++ws;
      if (job->live && !liveSubscribed(fds[i], now)) continue;
      // Telemetrie und Live-Daten kommen ohnehin gleich neu: kommt das WLAN nicht nach,
      // diese Nachricht für diesen Client auslassen statt zu warten
      if (!socketWritable(fds[i])) {
        g_ws_dropped.fetch_add(1);
        continue;
      }
      httpd_ws_frame_t f = {};
      f.type = HTTPD_WS_TYPE_TEXT;
      f.payload = (uint8_t*)job->data;
      f.len = job->len;
      const esp_err_t err = httpd_ws_send_frame_async(g_server, fds[i], &f);
      if (err != ESP_OK) {
        // evtl. nur ein Teil des Frames gesendet -> Verbindung ist unbrauchbar
        logWsSendFailure(job->live ? "Live-Ansicht" : "Telemetrie", err);
        httpd_sess_trigger_close(g_server, fds[i]);
        continue;
      }
    }
  }
  if (!job->live) g_ws_clients = ws;
  g_queued[job->live ? 1 : 0].fetch_sub(1);
  free(job->data);
  delete job;
}

void registerUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*),
                 bool websocket = false) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  u.is_websocket = websocket;
  if (httpd_register_uri_handler(g_server, &u) != ESP_OK) log_e("URI %s nicht registriert", uri);
}

}  // namespace

bool begin() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = HTTP_PORT;
  config.max_uri_handlers = 28;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.stack_size = 8192;
  config.core_id = 0;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&g_server, &config) != ESP_OK) {
    log_e("HTTP-Server konnte nicht gestartet werden");
    return false;
  }
  // Reihenfolge wichtig: spezifische Pfade vor dem Wildcard-Handler
  registerUri("/api/status", HTTP_GET, statusHandler);
  registerUri("/api/calibration", HTTP_GET, calibrationGetHandler);
  registerUri("/api/calibration", HTTP_POST, calibrationPostHandler);
  registerUri("/api/camera", HTTP_GET, cameraGetHandler);
  registerUri("/api/camera", HTTP_POST, cameraPostHandler);
  registerUri("/api/wifi", HTTP_GET, wifiGetHandler);
  registerUri("/api/wifi", HTTP_POST, wifiPostHandler);
  registerUri("/api/wifi/scan", HTTP_GET, wifiScanGetHandler);
  registerUri("/api/wifi/scan", HTTP_POST, wifiScanPostHandler);
  registerUri("/api/battery", HTTP_GET, batteryGetHandler);
  registerUri("/api/battery", HTTP_POST, batteryPostHandler);
  registerUri("/api/leds", HTTP_GET, ledsGetHandler);
  registerUri("/api/leds", HTTP_POST, ledsPostHandler);
  registerUri("/api/pad", HTTP_GET, padGetHandler);
  registerUri("/api/pad", HTTP_POST, padPostHandler);
  registerUri("/api/log", HTTP_GET, logHandler);
  registerUri("/api/history", HTTP_GET, historyHandler);
  registerUri("/api/ota", HTTP_POST, otaHandler);
  registerUri("/api/kinematics", HTTP_GET, kinematicsHandler);
  registerUri("/api/i2c", HTTP_GET, i2cGetHandler);
  registerUri("/api/i2c/scan", HTTP_POST, i2cScanHandler);
  registerUri("/api/estop", HTTP_POST, estopHandler);
  registerUri("/api/reboot", HTTP_POST, rebootHandler);
  registerUri("/ws", HTTP_GET, wsHandler, true);
  registerUri("/*", HTTP_GET, assetHandler);
  return true;
}

static void queueBroadcast(const String& msg, bool live) {
  if (!g_server || msg.isEmpty()) return;
  // Hängt der Server-Task noch an älteren Nachrichten, die neue nicht auch noch anstellen
  std::atomic<int>& queued = g_queued[live ? 1 : 0];
  if (queued.load() >= 2) {
    g_ws_dropped.fetch_add(1);
    return;
  }
  BroadcastJob* job = new BroadcastJob;
  job->live = live;
  job->len = msg.length();
  job->data = (char*)malloc(job->len);
  if (!job->data) {
    delete job;
    return;
  }
  memcpy(job->data, msg.c_str(), job->len);
  queued.fetch_add(1);
  if (httpd_queue_work(g_server, broadcastWork, job) != ESP_OK) {
    queued.fetch_sub(1);
    free(job->data);
    delete job;
  }
}

void broadcast(const String& msg) { queueBroadcast(msg, false); }
void broadcastLive(const String& msg) { queueBroadcast(msg, true); }
bool liveWanted() { return g_live_until && (int32_t)(g_live_until - millis()) > 0; }

int wsClientCount() { return g_ws_clients; }
uint32_t wsDropped() { return g_ws_dropped.load(); }

uint32_t msSinceLastCommand() { return millis() - g_last_cmd_ms; }

}  // namespace web
