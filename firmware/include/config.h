#pragma once
// Zentrale Hardware- und Systemkonfiguration des Hexapod MK3.
// Pinbelegung gemäß docs/hexapod_schaltplan_v3.png und Board-Doku
// (docs/ESP32_S3_Camera_Instructions.pdf, Seite "Product Pin Description").

#include <stdint.h>

#define HEXAPOD_FW_VERSION "0.1.0"

// ---------------------------------------------------------------------------
// I2C-Bus (PCA9685 #1, PCA9685 #2, MPU6050)
// ---------------------------------------------------------------------------
constexpr int      PIN_I2C_SDA  = 47;
constexpr int      PIN_I2C_SCL  = 21;
constexpr uint32_t I2C_FREQ_HZ  = 400000;

constexpr uint8_t  PCA1_ADDR    = 0x40;  // linke Seite  (Beine 1-3)
constexpr uint8_t  PCA2_ADDR    = 0x41;  // rechte Seite (Beine 4-6), A0 gebrückt
constexpr uint8_t  MPU6050_ADDR = 0x68;  // AD0 an GND

// Interner Oszillator der PCA9685. Nominal 25 MHz, real oft 24-27 MHz.
// Wer ein Oszilloskop/Logic-Analyzer hat: 1500 µs ausgeben, messen und hier
// korrigieren (Ist-Frequenz = 25e6 * gemessen / 1500). Die Servokalibrierung
// gleicht den Fehler ohnehin aus, nur der Rohmodus (µs) wird dadurch exakt.
constexpr float    PCA1_OSC_HZ  = 25000000.0f;
constexpr float    PCA2_OSC_HZ  = 25000000.0f;
constexpr float    SERVO_PWM_HZ = 50.0f;

// ---------------------------------------------------------------------------
// Sonstige GPIOs
// ---------------------------------------------------------------------------
constexpr int PIN_BATTERY_ADC = 1;   // Spannungsteiler R1 100k / R2 47k
constexpr int PIN_EYES_LED    = 14;  // 2x WS2812B "Augen" (über 330 Ω)
constexpr int PIN_BOARD_LED   = 48;  // WS2812 auf dem ESP32-S3-CAM-Board
constexpr int PIN_BOOT_BUTTON = 0;   // BOOT-Taster (low-aktiv)

// ---------------------------------------------------------------------------
// Kamera (ESP32-S3-CAM, OV2640/OV5640 – Typ wird automatisch erkannt)
// ---------------------------------------------------------------------------
constexpr int CAM_PIN_PWDN  = -1;
constexpr int CAM_PIN_RESET = -1;
constexpr int CAM_PIN_XCLK  = 15;
constexpr int CAM_PIN_SIOD  = 4;
constexpr int CAM_PIN_SIOC  = 5;
constexpr int CAM_PIN_Y9    = 16;
constexpr int CAM_PIN_Y8    = 17;
constexpr int CAM_PIN_Y7    = 18;
constexpr int CAM_PIN_Y6    = 12;
constexpr int CAM_PIN_Y5    = 10;
constexpr int CAM_PIN_Y4    = 8;
constexpr int CAM_PIN_Y3    = 9;
constexpr int CAM_PIN_Y2    = 11;
constexpr int CAM_PIN_VSYNC = 6;
constexpr int CAM_PIN_HREF  = 7;
constexpr int CAM_PIN_PCLK  = 13;
constexpr int CAM_XCLK_HZ   = 20000000;
// Kamera schläft, wenn so lange kein Stream/Abruf lief (OV5640 wird sonst sehr heiß)
constexpr uint32_t CAM_IDLE_SLEEP_MS = 10000;

// ---------------------------------------------------------------------------
// Akku (2S Li-Ion, OHNE BMS -> Software muss Tiefentladung verhindern!)
// ---------------------------------------------------------------------------
// Überwachung und Tiefschlaf lassen sich in der Weboberfläche (Tab "System")
// schalten; die Einstellung liegt im NVS. Dies sind nur die Werkseinstellungen.
// Ab Werk AUS: Ohne bestückten Spannungsteiler liefert GPIO 1 Zufallswerte,
// die sonst die Servos sperren oder den ESP schlafen legen könnten.
constexpr bool  BAT_MONITOR_DEFAULT    = false;
constexpr bool  BAT_DEEP_SLEEP_DEFAULT = true;   // wirkt nur bei aktiver Überwachung
constexpr float BAT_DIVIDER_RATIO = (100.0f + 47.0f) / 47.0f;  // ~3.128
constexpr float BAT_CELLS         = 2.0f;
constexpr float BAT_WARN_V        = 6.80f;   // 3.40 V/Zelle -> Warnung
constexpr float BAT_CRITICAL_V    = 6.40f;   // 3.20 V/Zelle -> Servos aus
constexpr float BAT_PRESENT_V     = 4.50f;   // darunter: kein Akku (USB-Betrieb)
constexpr uint32_t BAT_CRITICAL_HOLD_MS = 5000;  // so lange unter Grenze -> Servos aus
// Unterhalb dieser Spannung geht der ESP in den Tiefschlaf, damit er die
// Zellen nicht weiter entlädt (Aufwecken nur per Reset nach dem Laden).
constexpr float BAT_SHUTDOWN_V    = 6.00f;   // 3.00 V/Zelle
constexpr uint32_t BAT_SHUTDOWN_HOLD_MS = 10000;

// ---------------------------------------------------------------------------
// Servos / Beine
// ---------------------------------------------------------------------------
constexpr int NUM_LEGS          = 6;
constexpr int JOINTS_PER_LEG    = 3;
constexpr int NUM_SERVOS        = NUM_LEGS * JOINTS_PER_LEG;

// Absolute Grenzen im Rohmodus (schützt vor Endanschlag-Brummen)
constexpr float SERVO_ABS_MIN_US = 500.0f;
constexpr float SERVO_ABS_MAX_US = 2500.0f;
constexpr float SERVO_DEFAULT_US_PER_DEG = 2000.0f / 180.0f;  // MG90S ~11.1 µs/°

// Maximale Stellgeschwindigkeit (Rampen gegen Stromspitzen). MG90S schafft
// ca. 600 °/s; für Kalibrierung und Tests deutlich langsamer fahren.
constexpr float SERVO_SLEW_DEG_PER_S = 240.0f;

// Regelschleife
constexpr uint32_t CONTROL_PERIOD_MS = 20;  // 50 Hz = Servo-PWM-Rate
constexpr int      CONTROL_TASK_CORE = 1;   // WLAN/HTTP laufen auf Core 0
constexpr int      CONTROL_TASK_PRIO = 5;

// ---------------------------------------------------------------------------
// WLAN / Webserver
// ---------------------------------------------------------------------------
constexpr const char* WIFI_HOSTNAME   = "hexapod";      // -> http://hexapod.local
constexpr const char* WIFI_AP_SSID    = "Hexapod-MK3";
constexpr const char* WIFI_AP_PASS    = "hexapod123";   // min. 8 Zeichen (WPA2)
constexpr uint32_t    WIFI_STA_TIMEOUT_MS = 15000;     // Verbindungsversuch beim Start
constexpr uint32_t    WIFI_CONNECT_TIMEOUT_MS = 20000; // Verbindungsversuch aus der Oberfläche
constexpr uint32_t    WIFI_AP_LINGER_MS = 120000;      // AP läuft nach erfolgreicher Verbindung noch so lange
constexpr uint32_t    WIFI_RETRY_PERIOD_MS = 60000;    // im AP-Modus: Heimnetz erneut versuchen
constexpr uint32_t    WIFI_STA_LOST_FALLBACK_MS = 60000; // Heimnetz so lange weg -> AP starten
constexpr uint16_t    HTTP_PORT       = 80;
constexpr uint16_t    STREAM_PORT     = 81;
constexpr uint32_t    TELEMETRY_PERIOD_MS = 250;

// LED-Helligkeit begrenzen (Schaltplan: max. 20-30 %)
constexpr uint8_t LED_MAX_BRIGHTNESS = 64;  // von 255
