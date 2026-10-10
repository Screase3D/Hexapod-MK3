// Hexapod MK3 - Firmware-Einstieg
//
// Core 1: Regel-Task (50 Hz) -> IMU, Akku, Servo-Ausgabe (einziger I2C-Nutzer)
// Core 0: WLAN, HTTP-Server (Port 80), Kamera-Stream (Port 81)
// loop(): Telemetrie an die Weboberfläche, Status-LEDs, Sicherheitslogik

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>

#include "config.h"
#include "net/camera.h"
#include "net/commands.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "sensors/battery.h"
#include "sensors/imu.h"
#include "servo/servo_controller.h"
#include "ui/status_leds.h"

namespace {

constexpr uint32_t BATTERY_EVERY_N_CYCLES = 5;  // 10 Hz
constexpr uint32_t LED_PERIOD_MS = 40;

volatile bool g_battery_cutoff = false;

void controlTask(void*) {
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t cycle = 0;
  uint32_t last_us = micros();
  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    uint32_t now_us = micros();
    float dt = (now_us - last_us) / 1e6f;
    last_us = now_us;

    imu.update(dt);
    if (++cycle % BATTERY_EVERY_N_CYCLES == 0) g_battery_cutoff = battery.update();
    // Hier kommt in Phase 2 die Gangart / inverse Kinematik dazu.
    servos.update(dt);
  }
}

// Akku-Tiefentladeschutz: Servos sperren, bis die Spannung wieder steigt
void updateSafety() {
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
  StatusLeds::Mode mode;
  if (servos.emergencyStopActive()) {
    mode = StatusLeds::Mode::EmergencyStop;
  } else if (battery.state() == BatteryMonitor::State::Critical) {
    mode = StatusLeds::Mode::BatteryLow;
  } else if (battery.state() == BatteryMonitor::State::Warn) {
    mode = StatusLeds::Mode::BatteryWarn;
  } else if (servos.torqueRequested()) {
    mode = StatusLeds::Mode::TorqueOn;
  } else if (wifi::isAccessPoint()) {
    mode = StatusLeds::Mode::ApMode;
  } else {
    mode = StatusLeds::Mode::Idle;
  }
  statusLeds.setMode(mode);
  statusLeds.update();
}

void printBanner() {
  Serial.println();
  Serial.println("==============================");
  Serial.printf(" Hexapod MK3  Firmware %s\n", HEXAPOD_FW_VERSION);
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
  statusLeds.setMode(StatusLeds::Mode::Boot);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(20);

  servos.begin(Wire);
  Serial.printf("[Servo] PCA9685 #1 (0x%02X): %s, #2 (0x%02X): %s\n", PCA1_ADDR,
                servos.boardOk(0) ? "OK" : "FEHLT", PCA2_ADDR, servos.boardOk(1) ? "OK" : "FEHLT");

  bool imu_ok = imu.begin(Wire, MPU6050_ADDR);
  Serial.printf("[IMU]   MPU6050 (0x%02X): %s\n", MPU6050_ADDR, imu_ok ? "OK" : "FEHLT");

  battery.begin();
  Serial.printf("[Akku]  %.2f V\n", battery.voltage());

  bool cam_ok = camera::begin();
  Serial.printf("[Kamera] %s\n", cam_ok ? "OK" : "FEHLT");

  wifi::begin();
  web::begin();
  camera::startStreamServer();
  Serial.printf("[Web]   http://%s/  bzw.  http://%s.local/\n", wifi::ip().c_str(), WIFI_HOSTNAME);

  xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, CONTROL_TASK_PRIO, nullptr,
                          CONTROL_TASK_CORE);
  Serial.println("Bereit. Servos sind aus - Torque in der Weboberflaeche einschalten.");
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
  }

  wifi::loop();

  if (commands::rebootRequested()) {
    if (reboot_at == 0) reboot_at = now + 500;  // Antwort noch ausliefern
    if ((int32_t)(now - reboot_at) >= 0) ESP.restart();
  }

  delay(5);
}
