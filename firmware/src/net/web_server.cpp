#include "web_server.h"

#include <ArduinoJson.h>
#include <esp_http_server.h>

#include "config.h"
#include "generated/web_assets.h"
#include "net/camera.h"
#include "net/commands.h"
#include "net/wifi_manager.h"
#include "servo/servo_controller.h"

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
  doc["saved_ssid"] = wifi::savedSsid();
  return sendJson(req, doc);
}

esp_err_t wifiPostHandler(httpd_req_t* req) {
  JsonDocument doc;
  String err;
  if (!readJsonBody(req, doc, err)) return sendResult(req, false, err);
  if (doc["clear"] | false) {
    wifi::clearCredentials();
    return sendResult(req, true, "WLAN-Daten gelöscht - nach Neustart Access-Point-Modus");
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";
  if (!wifi::saveCredentials(ssid, pass)) {
    return sendResult(req, false, "SSID 1-32 Zeichen, Passwort leer oder 8-64 Zeichen");
  }
  return sendResult(req, true, "Gespeichert - wird nach Neustart verwendet");
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
  config.max_uri_handlers = 16;
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
