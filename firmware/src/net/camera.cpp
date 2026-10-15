#include "camera.h"

#include <Preferences.h>
#include <atomic>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

#include "config.h"
#include "net/cam_zoom.h"
#include "net/wifi_manager.h"

namespace camera {
namespace {

#define PART_BOUNDARY "hexapodframe"
constexpr const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
constexpr const char* STREAM_PART =
    "\r\n--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

constexpr const char* NVS_NAMESPACE = "camera";

bool g_ok = false;  // beim Start erkannt
httpd_handle_t g_httpd = nullptr;
std::atomic<int> g_stream_clients{0};
Settings g_settings = {FRAMESIZE_VGA, 12, false, false, 15, 0, CAM_XCLK_HZ / 1000000, FRAMESIZE_CIF,
                       0, 0, 0, 0, 0, GAINCEILING_8X, 0, false, true, true};
int g_applied_fs = -1;  // zuletzt am Sensor eingestellte Auflösung (-1 = nach dem Aufwecken neu)
// Zoom (nur im RAM: ändert sich oft, soll den Flash nicht abnutzen; nach dem Neustart 1×)
float g_zoom = 1.0f;
float g_applied_zoom = 1.0f;
bool g_bt_applied = false;  // Bluetooth-Auflösung gerade aktiv (unter g_mutex)
int g_xclk_active_mhz = 0;  // beim letzten Aufwecken tatsächlich verwendeter Takt

// Ruhemodus: g_mutex schützt Treiber-Init/Deinit, g_awake und g_users
SemaphoreHandle_t g_mutex = nullptr;
std::atomic<bool> g_awake{false};
int g_users = 0;                 // laufende Streams + /capture-Abrufe
uint32_t g_last_use_ms = 0;      // Zeitpunkt, zu dem der letzte Nutzer fertig war
uint16_t g_pid = 0;              // Sensor-Typ (beim Start ermittelt)
const char* g_sensor_name = "";  // für die Anzeige, auch während die Kamera schläft
std::atomic<uint32_t> g_wake_ms{0};      // Dauer des letzten Aufweckens
std::atomic<uint32_t> g_wake_count{0};
std::atomic<int> g_skip_frames{0};       // erste Bilder nach dem Aufwecken verwerfen

// Stream-Statistik (über ein 2-s-Fenster gemittelt)
std::atomic<float> g_stat_fps{0.0f};
std::atomic<uint32_t> g_stat_avg_bytes{0};
std::atomic<uint32_t> g_stat_kbps{0};
std::atomic<uint32_t> g_fb_errors{0};

bool validXclk(int mhz) { return mhz == 10 || mhz == 16 || mhz == 20; }

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
  int xclk = p.getInt("xclk", g_settings.xclk_mhz);
  if (validXclk(xclk)) g_settings.xclk_mhz = xclk;
  int btfs = p.getInt("btfs", g_settings.bt_framesize);
  g_settings.bt_framesize = btfs == 0 || validFramesize(btfs) ? btfs : g_settings.bt_framesize;
  g_settings.brightness = constrain(p.getInt("br", 0), -2, 2);
  g_settings.contrast = constrain(p.getInt("ct", 0), -2, 2);
  g_settings.saturation = constrain(p.getInt("sa", 0), -2, 2);
  g_settings.sharpness = constrain(p.getInt("sh", 0), -2, 2);
  g_settings.ae_level = constrain(p.getInt("ae", 0), -2, 2);
  g_settings.gainceiling = constrain(p.getInt("gc", g_settings.gainceiling), 0, 6);
  g_settings.wb_mode = constrain(p.getInt("wb", 0), 0, 4);
  g_settings.aec2 = p.getBool("aec2", g_settings.aec2);
  g_settings.lenc = p.getBool("lenc", g_settings.lenc);
  g_settings.bpc = p.getBool("bpc", g_settings.bpc);
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
  p.putInt("xclk", g_settings.xclk_mhz);
  p.putInt("btfs", g_settings.bt_framesize);
  p.putInt("br", g_settings.brightness);
  p.putInt("ct", g_settings.contrast);
  p.putInt("sa", g_settings.saturation);
  p.putInt("sh", g_settings.sharpness);
  p.putInt("ae", g_settings.ae_level);
  p.putInt("gc", g_settings.gainceiling);
  p.putInt("wb", g_settings.wb_mode);
  p.putBool("aec2", g_settings.aec2);
  p.putBool("lenc", g_settings.lenc);
  p.putBool("bpc", g_settings.bpc);
  p.end();
}

// Tatsächliche Auflösung: mit Bluetooth höchstens bt_framesize
bool btReduces() {
  return wifi::bluetoothActive() && g_settings.bt_framesize > 0 && g_settings.framesize > g_settings.bt_framesize;
}
int activeFramesize() { return btReduces() ? g_settings.bt_framesize : g_settings.framesize; }

// Zoom am Sensor: nur OV5640 (Bildausschnitt statt ganzem Sensor); sonst zoomt die Oberfläche
bool zoomHw() { return g_ok && g_pid == OV5640_PID; }
float zoomMax(int fs) {
  if (!zoomHw()) return 4.0f;  // Oberfläche vergrößert das Bild
  return camzoom::maxZoom(resolution[fs].width, resolution[fs].height, resolution[fs].aspect_ratio);
}
float zoomFor(int fs) {
  const float m = zoomMax(fs);
  return g_zoom < m ? g_zoom : m;
}

void applyToSensor() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  g_bt_applied = btReduces();
  // Auflösung nur bei Änderung setzen: der Sensor stellt dabei neu ein, das Bild hakt kurz
  const int fs = activeFramesize();
  const float z = zoomHw() && s->set_res_raw ? zoomFor(fs) : 1.0f;
  if (fs != g_applied_fs || z != g_applied_zoom) {
    // 1× bzw. neue Auflösung: Werte des Treibers (auch PLL); Zoom: mittiger Ausschnitt
    if (fs != g_applied_fs || z <= 1.0f) s->set_framesize(s, (framesize_t)fs);
    camzoom::Window w;
    const resolution_info_t& r = resolution[fs];
    if (z > 1.0f && camzoom::window(r.width, r.height, r.aspect_ratio, z, w)) {
      s->set_res_raw(s, w.sx, w.sy, w.ex, w.ey, w.ox, w.oy, w.tx, w.ty, r.width, r.height, w.scale, w.binning);
    }
    g_applied_fs = fs;
    g_applied_zoom = z;
  }
  s->set_quality(s, g_settings.quality);
  // Bild: nicht jeder Sensor kann alles (fehlende Funktion -> überspringen)
  auto set = [s](int (*fn)(sensor_t*, int), int v) {
    if (fn) fn(s, v);
  };
  set(s->set_brightness, g_settings.brightness);
  set(s->set_contrast, g_settings.contrast);
  set(s->set_saturation, g_settings.saturation);
  if (s->id.PID != OV2640_PID) set(s->set_sharpness, g_settings.sharpness);  // OV2640: nicht unterstützt
  set(s->set_exposure_ctrl, 1);
  set(s->set_gain_ctrl, 1);
  set(s->set_ae_level, g_settings.ae_level);
  set(s->set_aec2, g_settings.aec2 ? 1 : 0);
  if (s->set_gainceiling) s->set_gainceiling(s, (gainceiling_t)g_settings.gainceiling);
  set(s->set_whitebal, 1);
  set(s->set_awb_gain, 1);
  set(s->set_wb_mode, g_settings.wb_mode);
  set(s->set_lenc, g_settings.lenc ? 1 : 0);
  set(s->set_bpc, g_settings.bpc ? 1 : 0);
  set(s->set_wpc, g_settings.bpc ? 1 : 0);
  set(s->set_raw_gma, 1);
  // 180° = in beide Richtungen spiegeln; 270° = 180° hier + 90° in der UI
  bool flip180 = g_settings.rotation == 180 || g_settings.rotation == 270;
  s->set_vflip(s, (g_settings.vflip != flip180) ? 1 : 0);
  s->set_hmirror(s, (g_settings.hmirror != flip180) ? 1 : 0);
}

bool acquire();
void release();
camera_fb_t* getFrame();

esp_err_t sendUnavailable(httpd_req_t* req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "Kamera nicht verfügbar");
}

esp_err_t streamHandler(httpd_req_t* req) {
  if (!acquire()) return sendUnavailable(req);

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  ++g_stream_clients;
  char part[128];
  esp_err_t res = ESP_OK;
  int64_t last_frame_us = 0;
  int64_t fail_since_us = 0;
  int64_t window_start_us = esp_timer_get_time();
  uint32_t window_frames = 0, window_bytes = 0;

  while (res == ESP_OK) {
    // Bildrate begrenzen
    int64_t min_interval_us = 1000000LL / (g_settings.max_fps > 0 ? g_settings.max_fps : 15);
    int64_t wait_us = last_frame_us + min_interval_us - esp_timer_get_time();
    if (wait_us > 1000) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
    last_frame_us = esp_timer_get_time();

    camera_fb_t* fb = getFrame();
    if (!fb) {
      // Einzelne Aussetzer tolerieren, erst nach 3 s ohne Bild aufgeben
      ++g_fb_errors;
      int64_t now = esp_timer_get_time();
      if (fail_since_us == 0) fail_since_us = now;
      if (now - fail_since_us > 3000000LL) {
        log_e("Kamera liefert seit 3 s keine Bilder - Stream beendet");
        res = ESP_FAIL;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    fail_since_us = 0;
    if (fb->format == PIXFORMAT_JPEG) {
      // Trenner + Kopf in einem Paket, danach das Bild
      size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
      if (res == ESP_OK) {
        ++window_frames;
        window_bytes += fb->len;
      }
    }
    esp_camera_fb_return(fb);

    int64_t now = esp_timer_get_time();
    if (now - window_start_us >= 2000000LL) {
      float secs = (now - window_start_us) / 1e6f;
      g_stat_fps = window_frames / secs;
      g_stat_avg_bytes = window_frames ? window_bytes / window_frames : 0;
      g_stat_kbps = (uint32_t)(window_bytes * 8 / 1000 / secs);
      window_start_us = now;
      window_frames = window_bytes = 0;
    }
  }
  if (--g_stream_clients == 0) {
    g_stat_fps = 0;
    g_stat_kbps = 0;
  }
  release();
  return res;
}

esp_err_t captureHandler(httpd_req_t* req) {
  if (!acquire()) return sendUnavailable(req);
  camera_fb_t* fb = getFrame();
  if (!fb) {
    release();
    return sendUnavailable(req);
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=hexapod.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  release();
  return res;
}

// Startet Takt, Sensor und Bildaufnahme (Treiber-Init)
bool initDriver() {
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
  config.xclk_freq_hz = g_settings.xclk_mhz * 1000000;
  g_xclk_active_mhz = g_settings.xclk_mhz;
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
    return false;
  }
  return true;
}

// Software-Power-Down des Sensors (PWDN-Pin ist nicht verdrahtet)
void sensorStandby(bool standby) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  switch (g_pid) {
    case OV5640_PID:
    case OV3660_PID:
      s->set_reg(s, 0x3008, 0x40, standby ? 0x40 : 0x00);  // SYSTEM CTROL0, Bit 6
      break;
    case OV2640_PID:
      s->set_reg(s, 0x109, 0x10, standby ? 0x10 : 0x00);  // COM2 (Sensor-Bank), Bit 4
      break;
    default:
      break;  // nur Takt/Bildaufnahme aus
  }
}

// Ab hier g_mutex halten
bool wakeLocked() {
  if (g_awake) return true;
  uint32_t t0 = millis();
  if (!initDriver()) return false;
  g_applied_fs = -1;  // Sensor neu initialisiert
  sensorStandby(false);  // falls der Sensor-Reset das Bit nicht löscht
  applyToSensor();
  g_awake = true;
  g_skip_frames = 2;  // die ersten Bilder sind oft unvollständig/zu dunkel
  g_wake_ms = millis() - t0;
  ++g_wake_count;
  log_i("Kamera geweckt (%u ms)", (unsigned)g_wake_ms.load());
  return true;
}

void sleepLocked() {
  if (!g_awake) return;
  sensorStandby(true);
  esp_camera_deinit();
  g_awake = false;
  log_i("Kamera schläft");
}

// Kamera für einen Stream/Abruf belegen (weckt sie bei Bedarf)
bool acquire() {
  if (!g_ok) return false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  bool ok = wakeLocked();
  if (ok) ++g_users;
  xSemaphoreGive(g_mutex);
  return ok;
}

void release() {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (g_users > 0) --g_users;
  g_last_use_ms = millis();
  xSemaphoreGive(g_mutex);
}

camera_fb_t* getFrame() {
  camera_fb_t* fb = esp_camera_fb_get();
  while (fb && g_skip_frames.load() > 0) {
    --g_skip_frames;
    esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
  }
  return fb;
}

}  // namespace

bool begin() {
  loadSettings();
  g_mutex = xSemaphoreCreateMutex();
  if (!initDriver()) return false;
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    g_pid = s->id.PID;
    camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
    g_sensor_name = info ? info->name : "unbekannt";
  }
  g_ok = true;
  g_awake = true;
  // Gleich wieder schlafen legen – geweckt wird beim ersten Stream
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  sleepLocked();
  xSemaphoreGive(g_mutex);
  return true;
}

bool ok() { return g_ok; }

void loop() {
  if (!g_ok || !g_awake) return;
  // Nicht blockieren, falls gerade ein Stream die Kamera weckt
  if (xSemaphoreTake(g_mutex, 0) != pdTRUE) return;
  if (g_awake && g_users == 0 && millis() - g_last_use_ms >= CAM_IDLE_SLEEP_MS) sleepLocked();
  // Bluetooth an/aus: Auflösung anpassen (läuft der Stream, sofort)
  if (g_awake && btReduces() != g_bt_applied) applyToSensor();
  xSemaphoreGive(g_mutex);
}

bool startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = STREAM_PORT;
  config.ctrl_port = 32769;  // muss sich vom Server auf Port 80 unterscheiden
  config.max_open_sockets = 2;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;
  config.core_id = 0;
  config.stack_size = 6144;
  // Bei schwachem WLAN können einzelne Pakete länger hängen – nicht sofort abbrechen
  config.send_wait_timeout = 10;
  config.recv_wait_timeout = 10;

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
  // Zoom allein (Mausrad, Knopf): sofort, ohne die Einstellungen zu speichern
  float zoom = g_zoom;
  if (!obj["zoom"].isNull()) {
    zoom = obj["zoom"].as<float>();
    if (!(zoom >= 1.0f)) zoom = 1.0f;
    if (zoom > 4.0f) zoom = 4.0f;
    zoom = roundf(zoom * 20.0f) / 20.0f;
  }
  if (obj.is<JsonObjectConst>() && obj.as<JsonObjectConst>().size() == 1 && !obj["zoom"].isNull()) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_zoom = fminf(zoom, zoomMax(activeFramesize()));
    if (g_awake) applyToSensor();
    xSemaphoreGive(g_mutex);
    return true;
  }
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
  if (obj["xclk_mhz"].is<int>()) {
    int mhz = obj["xclk_mhz"];
    if (!validXclk(mhz)) {
      err = "XCLK muss 10, 16 oder 20 MHz sein";
      return false;
    }
    next.xclk_mhz = mhz;
  }
  if (obj["rotation"].is<int>()) {
    int rot = obj["rotation"];
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
      err = "Drehung muss 0, 90, 180 oder 270 sein";
      return false;
    }
    next.rotation = rot;
  }
  auto level = [&](const char* key, int& out, int lo, int hi) {
    if (obj[key].is<int>()) out = constrain(obj[key].as<int>(), lo, hi);
  };
  level("brightness", next.brightness, -2, 2);
  level("contrast", next.contrast, -2, 2);
  level("saturation", next.saturation, -2, 2);
  level("sharpness", next.sharpness, -2, 2);
  level("ae_level", next.ae_level, -2, 2);
  level("gainceiling", next.gainceiling, 0, 6);
  level("wb_mode", next.wb_mode, 0, 4);
  if (obj["aec2"].is<bool>()) next.aec2 = obj["aec2"];
  if (obj["lenc"].is<bool>()) next.lenc = obj["lenc"];
  if (obj["bpc"].is<bool>()) next.bpc = obj["bpc"];
  if (obj["bt_framesize"].is<int>()) {
    int fs = obj["bt_framesize"];
    if (fs != 0 && !validFramesize(fs)) {
      err = "ungültige Auflösung für Bluetooth";
      return false;
    }
    next.bt_framesize = fs;
  }
  // Nicht gleichzeitig mit Aufwecken/Schlafenlegen; schläft die Kamera,
  // gelten die Werte beim nächsten Aufwecken
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_settings = next;
  g_zoom = fminf(zoom, zoomMax(g_settings.framesize));  // größere Auflösung: Zoom bleibt dort begrenzt
  if (g_awake) applyToSensor();
  xSemaphoreGive(g_mutex);
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
  obj["xclk_mhz"] = g_settings.xclk_mhz;
  obj["xclk_active_mhz"] = g_xclk_active_mhz;
  obj["bt_framesize"] = g_settings.bt_framesize;
  JsonObject img = obj["image"].to<JsonObject>();
  img["brightness"] = g_settings.brightness;
  img["contrast"] = g_settings.contrast;
  img["saturation"] = g_settings.saturation;
  img["sharpness"] = g_settings.sharpness;
  img["ae_level"] = g_settings.ae_level;
  img["gainceiling"] = g_settings.gainceiling;
  img["wb_mode"] = g_settings.wb_mode;
  img["aec2"] = g_settings.aec2;
  img["lenc"] = g_settings.lenc;
  img["bpc"] = g_settings.bpc;
  obj["sharpness_ok"] = g_ok && g_pid != OV2640_PID;  // Schärfe einstellbar
  obj["framesize_active"] = activeFramesize();
  // Zoom: zoom_hw = Sensor schneidet aus (echte Details), sonst vergrößert die Oberfläche
  obj["zoom"] = roundf(zoomFor(activeFramesize()) * 100.0f) / 100.0f;
  obj["zoom_max"] = roundf(zoomMax(activeFramesize()) * 100.0f) / 100.0f;
  obj["zoom_hw"] = zoomHw();
  obj["clients"] = g_stream_clients.load();
  obj["port"] = STREAM_PORT;
  JsonArray sizes = obj["framesizes"].to<JsonArray>();
  for (const auto& f : FRAMESIZES) {
    if (!psramFound() && f.id > FRAMESIZE_VGA) break;
    JsonObject o = sizes.add<JsonObject>();
    o["id"] = (int)f.id;
    o["name"] = f.name;
  }
  if (g_ok) obj["sensor"] = g_sensor_name;
  obj["awake"] = g_awake.load();
  obj["idle_sleep_s"] = CAM_IDLE_SLEEP_MS / 1000;
}

void statsToJson(JsonObject obj) {
  int clients = g_stream_clients.load();
  obj["clients"] = clients;
  obj["err"] = g_fb_errors.load();
  obj["awake"] = g_awake.load();
  obj["wake_ms"] = g_wake_ms.load();
  obj["wakes"] = g_wake_count.load();
  if (btReduces()) obj["bt_fs"] = g_settings.bt_framesize;  // Bluetooth: kleinere Auflösung aktiv
  if (clients == 0) return;
  obj["fps"] = roundf(g_stat_fps.load() * 10.0f) / 10.0f;
  obj["kb"] = (g_stat_avg_bytes.load() + 512) / 1024;
  obj["kbps"] = g_stat_kbps.load();
}

}  // namespace camera
