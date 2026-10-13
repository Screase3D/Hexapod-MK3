#include "web_server.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_server.h>

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
  while (remaining > 0) {
    int r = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      err = "Lesefehler";
      return false;
    }
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

esp_err_t wsHandler(httpd_req_t* req) {
  if (req->method == HTTP_GET) return ESP_OK;  // Handshake abgeschlossen

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
  return httpd_ws_send_frame(req, &resp);
}

struct BroadcastJob {
  char* data;
  size_t len;
};

// Läuft im Kontext des HTTP-Server-Tasks (httpd_queue_work) -> thread-sicher
void broadcastWork(void* arg) {
  BroadcastJob* job = static_cast<BroadcastJob*>(arg);
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  size_t count = CONFIG_LWIP_MAX_SOCKETS;
  int ws = 0;
  if (httpd_get_client_list(g_server, &count, fds) == ESP_OK) {
    for (size_t i = 0; i < count; ++i) {
      if (httpd_ws_get_fd_info(g_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
      httpd_ws_frame_t f = {};
      f.type = HTTPD_WS_TYPE_TEXT;
      f.payload = (uint8_t*)job->data;
      f.len = job->len;
      httpd_ws_send_frame_async(g_server, fds[i], &f);
      ++ws;
    }
  }
  g_ws_clients = ws;
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

void broadcast(const String& msg) {
  if (!g_server || msg.isEmpty()) return;
  BroadcastJob* job = new BroadcastJob;
  job->len = msg.length();
  job->data = (char*)malloc(job->len);
  if (!job->data) {
    delete job;
    return;
  }
  memcpy(job->data, msg.c_str(), job->len);
  if (httpd_queue_work(g_server, broadcastWork, job) != ESP_OK) {
    free(job->data);
    delete job;
  }
}

int wsClientCount() { return g_ws_clients; }

uint32_t msSinceLastCommand() { return millis() - g_last_cmd_ms; }

}  // namespace web
