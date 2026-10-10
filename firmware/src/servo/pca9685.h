#pragma once
// Minimaler PCA9685-Treiber (16 Kanal, 12 Bit PWM) für Servos.
// Schreibt mehrere Kanäle in einer einzigen I2C-Transaktion (Auto-Increment).

#include <Arduino.h>
#include <Wire.h>

class Pca9685 {
 public:
  static constexpr int kChannels = 16;

  Pca9685(TwoWire& wire, uint8_t addr, float osc_hz)
      : wire_(wire), addr_(addr), osc_hz_(osc_hz) {}

  // Initialisiert den Baustein; alle Ausgänge bleiben aus.
  bool begin(float pwm_hz);
  bool isPresent();
  // false, wenn der Baustein nicht antwortet oder seit begin() einen Reset
  // hatte (MODE1.SLEEP gesetzt) -> dann erneut begin() aufrufen.
  bool healthy();
  bool ok() const { return ok_; }
  uint8_t address() const { return addr_; }

  // Pulsbreite in µs -> 12-Bit-Ticks (abhängig von Prescaler und Oszillator)
  uint16_t usToTicks(float us) const;

  // Schreibt `count` Kanäle ab `first`. ticks[i] == 0 -> Kanal komplett aus.
  // Die Pulsanfänge werden pro Kanal versetzt, damit nicht alle Servos
  // gleichzeitig Strom ziehen.
  bool writeChannels(uint8_t first, uint8_t count, const uint16_t* ticks);

  // Alle 16 Ausgänge sofort aus (Servos stromlos / "limp").
  bool allOff();

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t& val);

  TwoWire& wire_;
  uint8_t addr_;
  float osc_hz_;
  float pwm_hz_ = 50.0f;  // tatsächlich eingestellte Frequenz (nach Prescaler)
  bool ok_ = false;
};
