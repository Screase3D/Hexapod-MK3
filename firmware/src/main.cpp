// Hexapod MK3 - Firmware-Einstieg
//
// Core 1: Regel-Task (50 Hz) -> IMU, Akku, Servo-Ausgabe (einziger I2C-Nutzer)
// Core 0: WLAN, HTTP-Server (Port 80), Kamera-Stream (Port 81)
// loop(): Telemetrie an die Weboberfläche, Status-LEDs, Sicherheitslogik

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "config.h"
#include "control_stats.h"
#include "input/pad_ble.h"
#include "input/pad_control.h"
#include "motion/motion.h"
#include "net/camera.h"
#include "net/commands.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "sensors/battery.h"
#include "sensors/i2c_scan.h"
#include "sensors/imu.h"
#include "servo/servo_controller.h"
#include "sys/evlog.h"
#include "sys/ota.h"
#include "ui/status_leds.h"

namespace {

constexpr uint32_t BATTERY_EVERY_N_CYCLES = 5;  // 10 Hz
constexpr uint32_t LED_PERIOD_MS = 40;

volatile bool g_battery_cutoff = false;

void controlTask(void*) {
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t cycle = 0;
  uint32_t last_us = micros();
  uint32_t stats_start = millis(), stats_cycles = 0, stats_max_us = 0;
  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    uint32_t now_us = micros();
    float dt = (now_us - last_us) / 1e6f;
    last_us = now_us;

    i2cscan::process(Wire);  // nur wenn über die Weboberfläche angefordert
    imu.update(dt);
    if (++cycle % BATTERY_EVERY_N_CYCLES == 0) g_battery_cutoff = battery.update();
    motion::update(dt);  // im Pose-Modus: Körperhaltung -> IK -> Servo-Sollwerte
    servos.update(dt);

    // Statistik: Zyklen pro Sekunde und längste Rechenzeit
    uint32_t work_us = micros() - now_us;
    if (work_us > stats_max_us) stats_max_us = work_us;
    ++stats_cycles;
    uint32_t now_ms = millis();
    if (now_ms - stats_start >= 1000) {
      control_stats::hz = stats_cycles * 1000.0f / (now_ms - stats_start);
      control_stats::max_work_us = stats_max_us;
      stats_start = now_ms;
      stats_cycles = 0;
      stats_max_us = 0;
    }
  }
}

// Akku-Tiefentladeschutz (2S ohne BMS):
//  1. kritisch  -> Servos sperren, bis die Spannung wieder steigt
//  2. sehr tief -> Tiefschlaf, damit der ESP die Zellen nicht weiter leert
void updateSafety() {
  if (battery.shutdownRequired()) {
    Serial.printf("[Akku] %.2f V - Tiefschlaf zum Schutz der Zellen. Laden, dann Reset.\n",
                  battery.voltage());
    servos.emergencyStop();
    delay(3 * CONTROL_PERIOD_MS);  // Regel-Task schaltet die PCA-Ausgänge ab
    statusLeds.off();
    WiFi.mode(WIFI_OFF);
    Serial.flush();
    esp_deep_sleep_start();  // ohne Weckquelle: nur Reset/Neustart weckt auf
  }

  static bool inhibited = false;
  if (g_battery_cutoff && !inhibited) {
    servos.setInhibit("Akku leer");
    inhibited = true;
    Serial.println("[Akku] Unterspannung - Servos abgeschaltet");
  } else if (inhibited && battery.state() != BatteryMonitor::State::Critical) {
    servos.setInhibit("");
    inhibited = false;
  }
}

void updateLeds() {
  using ledfx::State;
  State st;
  if (servos.emergencyStopActive()) {
    st = State::Estop;
  } else if (battery.state() == BatteryMonitor::State::Critical) {
    st = State::BatteryLow;
  } else if (battery.state() == BatteryMonitor::State::Warn) {
    st = State::BatteryWarn;
  } else if (servos.torqueRequested()) {
    const motion::Activity a = motion::activity();
    if (a.arms) st = State::Arms;
    else if (a.gesture) st = State::Gesture;
    else if (a.walking) st = State::Walk;
    else if (a.standing) st = State::Stand;
    else st = State::Torque;
  } else if (wifi::isAccessPoint()) {
    st = State::Ap;
  } else {
    st = State::Idle;
  }
  statusLeds.setState(st);
  statusLeds.update();
}

// Zustandswechsel ins Ereignisprotokoll (Not-Aus, Servos, Akku, WLAN, Lagesensor, Controller)
void watchEvents() {
  static bool first = true;
  static bool estop, torque, sta, ap, imu_ok, lvl_fault;
  static BatteryMonitor::State bat;
  static padble::State ble;
  const bool estop_now = servos.emergencyStopActive();
  const bool torque_now = servos.torqueRequested();
  const BatteryMonitor::State bat_now = battery.state();
  const bool sta_now = wifi::staConnected();
  const bool ap_now = wifi::isAccessPoint();
  const bool imu_now = imu.ok();
  const bool lvl_now = motion::activity().level_fault;
  const padble::State ble_now = padble::status().state;
  if (!first) {
    if (estop_now != estop) {
      if (estop_now) EV_WARN("Not-Aus ausgelöst");
      else EV_INFO("Not-Aus freigegeben");
    }
    if (torque_now != torque && !estop_now) EV_INFO("Servos %s", torque_now ? "an" : "aus");
    if (bat_now != bat) {
      if (bat_now == BatteryMonitor::State::Warn) EV_WARN("Akku schwach: %.2f V", battery.voltage());
      else if (bat_now == BatteryMonitor::State::Critical) EV_ERROR("Akku leer: %.2f V – Servos gesperrt", battery.voltage());
      else if (bat == BatteryMonitor::State::Warn || bat == BatteryMonitor::State::Critical)
        EV_INFO("Akku wieder in Ordnung: %.2f V", battery.voltage());
    }
    if (sta_now != sta) {
      if (sta_now) EV_INFO("WLAN verbunden: %s (%s, %d dBm)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      else EV_WARN("WLAN getrennt");
    }
    if (ap_now != ap) EV_INFO("Access Point %s", ap_now ? "an" : "aus");
    if (imu_now != imu_ok) {
      if (imu_now) EV_INFO("Lagesensor antwortet wieder");
      else EV_ERROR("Lagesensor antwortet nicht");
    }
    if (lvl_now && !lvl_fault) EV_WARN("Lageausgleich abgeschaltet: Einbaulage des Lagesensors prüfen");
    if (ble_now != ble) {
      const padble::Status st = padble::status();
      switch (ble_now) {
        case padble::State::Connected: EV_INFO("Controller verbunden: %s", st.name); break;
        case padble::State::Pairing:   EV_INFO("Controller: Kopplung gestartet"); break;
        case padble::State::Searching:
          if (ble == padble::State::Connected) EV_WARN("Controller getrennt");
          break;
        case padble::State::Off:       EV_INFO("Bluetooth aus"); break;
        default: break;
      }
    }
  }
  first = false;
  estop = estop_now;
  torque = torque_now;
  bat = bat_now;
  sta = sta_now;
  ap = ap_now;
  imu_ok = imu_now;
  lvl_fault = lvl_now;
  ble = ble_now;
}

void printBanner() {
  Serial.println();
  Serial.println("==============================");
  Serial.printf(" Hexapod MK3  Firmware %s (%s, %s)\n", HEXAPOD_FW_VERSION, HEXAPOD_FW_GIT, HEXAPOD_FW_BUILT);
  Serial.println("==============================");
  Serial.printf("Chip %s rev %d, %d MHz, PSRAM %u KB\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getCpuFreqMHz(), (unsigned)(ESP.getPsramSize() / 1024));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  printBanner();

  statusLeds.begin();
  statusLeds.setState(ledfx::State::Boot);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(20);

  servos.begin(Wire);
  motion::begin();
  Serial.printf("[Servo] PCA9685 #1 (0x%02X): %s, #2 (0x%02X): %s\n", PCA1_ADDR,
                servos.boardOk(0) ? "OK" : "FEHLT", PCA2_ADDR, servos.boardOk(1) ? "OK" : "FEHLT");

  bool imu_ok = imu.begin(Wire, MPU6050_ADDR);
  Serial.printf("[IMU]   MPU6050 (0x%02X): %s\n", MPU6050_ADDR, imu_ok ? "OK" : "FEHLT");
  i2cscan::scanNow(Wire);

  battery.begin();
  Serial.printf("[Akku]  %.2f V (Überwachung %s, Tiefschlaf %s)\n", battery.voltage(),
                battery.enabled() ? "an" : "aus", battery.deepSleepEnabled() ? "an" : "aus");

  bool cam_ok = camera::begin();
  Serial.printf("[Kamera] %s\n", cam_ok ? "OK (schläft bis zum ersten Stream)" : "FEHLT");

  wifi::begin();
  web::begin();
  camera::startStreamServer();
  padctl::begin();  // Controller (Bluetooth nur, wenn eingeschaltet)
  ota::begin();     // Firmware-Update per PlatformIO über WLAN
  Serial.printf("[Web]   http://%s/  bzw.  http://%s.local/\n", wifi::ip().c_str(), WIFI_HOSTNAME);

  xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, CONTROL_TASK_PRIO, nullptr,
                          CONTROL_TASK_CORE);
  Serial.println("Bereit. Servos sind aus - Torque in der Weboberfläche einschalten.");
  bool reset_warn;
  const char* reason = evlog::resetReason(reset_warn);
  evlog::add(reset_warn ? evlog::Level::Warn : evlog::Level::Info, "Start: Firmware %s (%s), %s", HEXAPOD_FW_VERSION,
             HEXAPOD_FW_GIT, reason);
  if (!servos.boardOk(0) || !servos.boardOk(1)) {
    EV_ERROR("Servotreiber fehlt: PCA9685 #1 %s, #2 %s", servos.boardOk(0) ? "OK" : "FEHLT",
             servos.boardOk(1) ? "OK" : "FEHLT");
  }
  if (!imu_ok) EV_ERROR("Lagesensor (MPU6050) nicht gefunden");
  if (!cam_ok) EV_ERROR("Kamera nicht gefunden");
  if (wifi::staConnected()) {
    EV_INFO("WLAN verbunden: %s (%s, %d dBm)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (wifi::isAccessPoint()) {
    EV_INFO("Access Point aktiv (%s)", WiFi.softAPIP().toString().c_str());
  }
}

void loop() {
  static uint32_t last_telemetry = 0;
  static uint32_t last_led = 0;
  static uint32_t reboot_at = 0;
  uint32_t now = millis();

  updateSafety();

  if (now - last_telemetry >= TELEMETRY_PERIOD_MS) {
    last_telemetry = now;
    JsonDocument doc;
    commands::buildTelemetry(doc);
    String msg;
    serializeJson(doc, msg);
    web::broadcast(msg);
  }

  if (now - last_led >= LED_PERIOD_MS) {
    last_led = now;
    updateLeds();
    watchEvents();
  }

  wifi::loop();
  camera::loop();  // Kamera ohne Stream schlafen legen
  imu.loop();      // geänderte Lagesensor-Kalibrierung speichern
  motion::loop();  // geänderte Haltung/Gang verzögert speichern
  padctl::loop();  // Controller auswerten
  ota::loop();

  if (commands::rebootRequested()) {
    if (reboot_at == 0) reboot_at = now + 500;  // Antwort noch ausliefern
    if ((int32_t)(now - reboot_at) >= 0) ESP.restart();
  }

  delay(5);
}
