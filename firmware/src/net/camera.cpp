#include "camera.h"

#include <Preferences.h>
#include <atomic>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>

#include "config.h"

namespace camera {
namespace {

#define PART_BOUNDARY "hexapodframe"
constexpr const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
constexpr const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
constexpr const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

constexpr const char* NVS_NAMESPACE = "camera";

bool g_ok = false;
httpd_handle_t g_httpd = nullptr;
std::atomic<int> g_stream_clients{0};
Settings g_settings = {FRAMESIZE_VGA, 12, false, false, 15, 0};

struct FramesizeInfo {
  framesize_t id;
  const char* name;
};
constexpr FramesizeInfo FRAMESIZES[] = {
    {FRAMESIZE_QVGA, "QVGA 320×240"},  {FRAMESIZE_CIF, "CIF 400×296"},
    {FRAMESIZE_VGA, "VGA 640×480"},    {FRAMESIZE_SVGA, "SVGA 800×600"},
    {FRAMESIZE_XGA, "XGA 1024×768"},   {FRAMESIZE_HD, "HD 1280×720"},
    {FRAMESIZE_SXGA, "SXGA 1280×1024"}, {FRAMESIZE_UXGA, "UXGA 1600×1200"},
};

bool validFramesize(int fs) {
  for (const auto& f : FRAMESIZES) {
    if (f.id == fs) return true;
  }
  return false;
}

void loadSettings() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return;
  int fs = p.getInt("fs", g_settings.framesize);
  if (validFramesize(fs)) g_settings.framesize = fs;
  g_settings.quality = constrain(p.getInt("q", g_settings.quality), 4, 63);
  g_settings.vflip = p.getBool("vflip", g_settings.vflip);
  g_settings.hmirror = p.getBool("hmirror", g_settings.hmirror);
  g_settings.max_fps = constrain(p.getInt("fps", g_settings.max_fps), 1, 30);
  int rot = p.getInt("rot", 0);
  g_settings.rotation = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
  p.end();
}

void saveSettings() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return;
  p.putInt("fs", g_settings.framesize);
  p.putInt("q", g_settings.quality);
  p.putBool("vflip", g_settings.vflip);
  p.putBool("hmirror", g_settings.hmirror);
  p.putInt("fps", g_settings.max_fps);
  p.putInt("rot", g_settings.rotation);
  p.end();
}

void applyToSensor() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, (framesize_t)g_settings.framesize);
  s->set_quality(s, g_settings.quality);
  // 180° = in beide Richtungen spiegeln; 270° = 180° hier + 90° in der UI
  bool flip180 = g_settings.rotation == 180 || g_settings.rotation == 270;
  s->set_vflip(s, (g_settings.vflip != flip180) ? 1 : 0);
  s->set_hmirror(s, (g_settings.hmirror != flip180) ? 1 : 0);
}

esp_err_t sendUnavailable(httpd_req_t* req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "Kamera nicht verfügbar");
}

esp_err_t streamHandler(httpd_req_t* req) {
  if (!g_ok) return sendUnavailable(req);

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  ++g_stream_clients;
  char part[96];
  esp_err_t res = ESP_OK;
  int64_t last_frame_us = 0;
  int fail_count = 0;

  while (res == ESP_OK) {
    // Bildrate begrenzen
    int64_t min_interval_us = 1000000LL / (g_settings.max_fps > 0 ? g_settings.max_fps : 15);
    int64_t wait_us = last_frame_us + min_interval_us - esp_timer_get_time();
    if (wait_us > 1000) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
    last_frame_us = esp_timer_get_time();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      if (++fail_count > 20) res = ESP_FAIL;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    fail_count = 0;
    if (fb->format == PIXFORMAT_JPEG) {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) {
        size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, part, hlen);
      }
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
  }
  --g_stream_clients;
  return res;
}

esp_err_t captureHandler(httpd_req_t* req) {
  if (!g_ok) return sendUnavailable(req);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return sendUnavailable(req);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=hexapod.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

}  // namespace

bool begin() {
  loadSettings();

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_Y2;
  config.pin_d1 = CAM_PIN_Y3;
  config.pin_d2 = CAM_PIN_Y4;
  config.pin_d3 = CAM_PIN_Y5;
  config.pin_d4 = CAM_PIN_Y6;
  config.pin_d5 = CAM_PIN_Y7;
  config.pin_d6 = CAM_PIN_Y8;
  config.pin_d7 = CAM_PIN_Y9;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.sccb_i2c_port = 1;  // Port 0 gehört Wire (Servos/IMU)
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = CAM_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;
  // Mit größter Auflösung initialisieren, damit später jede Auflösung
  // in den Framebuffer passt; danach auf die gespeicherte umschalten.
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = g_settings.quality;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    log_w("Kein PSRAM - Kamera mit reduzierter Auflösung");
    config.frame_size = FRAMESIZE_VGA;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    if (g_settings.framesize > FRAMESIZE_VGA) g_settings.framesize = FRAMESIZE_VGA;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    log_e("Kamera-Init fehlgeschlagen: 0x%x", err);
    g_ok = false;
    return false;
  }
  applyToSensor();
  g_ok = true;
  return true;
}

bool ok() { return g_ok; }

bool startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = STREAM_PORT;
  config.ctrl_port = 32769;  // muss sich vom Server auf Port 80 unterscheiden
  config.max_open_sockets = 2;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;
  config.core_id = 0;
  config.stack_size = 6144;

  if (httpd_start(&g_httpd, &config) != ESP_OK) {
    log_e("Stream-Server konnte nicht gestartet werden");
    return false;
  }
  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream";
  stream_uri.method = HTTP_GET;
  stream_uri.handler = streamHandler;
  httpd_register_uri_handler(g_httpd, &stream_uri);

  httpd_uri_t capture_uri = {};
  capture_uri.uri = "/capture";
  capture_uri.method = HTTP_GET;
  capture_uri.handler = captureHandler;
  httpd_register_uri_handler(g_httpd, &capture_uri);
  return true;
}

Settings settings() { return g_settings; }

bool applyJson(JsonVariantConst obj, String& err) {
  Settings next = g_settings;
  if (obj["framesize"].is<int>()) {
    int fs = obj["framesize"];
    if (!validFramesize(fs)) {
      err = "ungültige Auflösung";
      return false;
    }
    if (!psramFound() && fs > FRAMESIZE_VGA) {
      err = "ohne PSRAM max. VGA";
      return false;
    }
    next.framesize = fs;
  }
  if (obj["quality"].is<int>()) next.quality = constrain(obj["quality"].as<int>(), 4, 63);
  if (obj["vflip"].is<bool>()) next.vflip = obj["vflip"];
  if (obj["hmirror"].is<bool>()) next.hmirror = obj["hmirror"];
  if (obj["max_fps"].is<int>()) next.max_fps = constrain(obj["max_fps"].as<int>(), 1, 30);
  if (obj["rotation"].is<int>()) {
    int rot = obj["rotation"];
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
      err = "Drehung muss 0, 90, 180 oder 270 sein";
      return false;
    }
    next.rotation = rot;
  }
  g_settings = next;
  if (g_ok) applyToSensor();
  saveSettings();
  return true;
}

void toJson(JsonObject obj) {
  obj["ok"] = g_ok;
  obj["framesize"] = g_settings.framesize;
  obj["quality"] = g_settings.quality;
  obj["vflip"] = g_settings.vflip;
  obj["hmirror"] = g_settings.hmirror;
  obj["max_fps"] = g_settings.max_fps;
  obj["rotation"] = g_settings.rotation;
  obj["clients"] = g_stream_clients.load();
  obj["port"] = STREAM_PORT;
  JsonArray sizes = obj["framesizes"].to<JsonArray>();
  for (const auto& f : FRAMESIZES) {
    if (!psramFound() && f.id > FRAMESIZE_VGA) break;
    JsonObject o = sizes.add<JsonObject>();
    o["id"] = (int)f.id;
    o["name"] = f.name;
  }
  if (g_ok) {
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
      camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
      obj["sensor"] = info ? info->name : "unbekannt";
    }
  }
}

}  // namespace camera
