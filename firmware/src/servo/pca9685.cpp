#include "pca9685.h"

namespace {
constexpr uint8_t REG_MODE1       = 0x00;
constexpr uint8_t REG_MODE2       = 0x01;
constexpr uint8_t REG_LED0_ON_L   = 0x06;
constexpr uint8_t REG_ALL_OFF_L   = 0xFC;
constexpr uint8_t REG_PRESCALE    = 0xFE;

constexpr uint8_t MODE1_RESTART = 0x80;
constexpr uint8_t MODE1_AI      = 0x20;  // Auto-Increment
constexpr uint8_t MODE1_SLEEP   = 0x10;
constexpr uint8_t MODE2_OUTDRV  = 0x04;  // Totem-Pole-Ausgänge

constexpr uint8_t FULL_OFF_BIT  = 0x10;  // in LEDn_OFF_H

// Versatz der Pulsanfänge je Kanal (in Ticks). 9 Kanäle * 400 Ticks
// verteilen sich bei 50 Hz über ~18 ms der 20-ms-Periode.
constexpr uint16_t PHASE_STEP_TICKS = 400;
}  // namespace

bool Pca9685::begin(float pwm_hz) {
  ok_ = false;
  if (!isPresent()) return false;

  // Prescaler nur im Sleep-Modus änderbar
  float prescale_f = osc_hz_ / (4096.0f * pwm_hz) - 1.0f;
  int prescale = constrain((int)lroundf(prescale_f), 3, 255);
  pwm_hz_ = osc_hz_ / (4096.0f * (prescale + 1));

  if (!writeReg(REG_MODE1, MODE1_SLEEP | MODE1_AI)) return false;
  if (!writeReg(REG_PRESCALE, (uint8_t)prescale)) return false;
  if (!writeReg(REG_MODE2, MODE2_OUTDRV)) return false;
  if (!allOff()) return false;
  if (!writeReg(REG_MODE1, MODE1_AI)) return false;  // aufwecken
  delayMicroseconds(600);                             // Oszillator stabil (>500 µs)
  uint8_t mode1 = 0;
  if (readReg(REG_MODE1, mode1) && (mode1 & MODE1_RESTART)) {
    writeReg(REG_MODE1, MODE1_AI | MODE1_RESTART);
  }
  ok_ = true;
  return true;
}

bool Pca9685::isPresent() {
  wire_.beginTransmission(addr_);
  return wire_.endTransmission() == 0;
}

bool Pca9685::healthy() {
  uint8_t mode1 = 0;
  if (!readReg(REG_MODE1, mode1)) {
    ok_ = false;
    return false;
  }
  if (mode1 & MODE1_SLEEP) {
    ok_ = false;
    return false;
  }
  return true;
}

uint16_t Pca9685::usToTicks(float us) const {
  float ticks = us * pwm_hz_ * 4096.0f / 1e6f;
  if (ticks < 1.0f) ticks = 1.0f;
  if (ticks > 4095.0f) ticks = 4095.0f;
  return (uint16_t)lroundf(ticks);
}

bool Pca9685::writeChannels(uint8_t first, uint8_t count, const uint16_t* ticks) {
  if (first + count > kChannels) return false;
  wire_.beginTransmission(addr_);
  wire_.write(REG_LED0_ON_L + 4 * first);
  for (uint8_t i = 0; i < count; ++i) {
    uint8_t ch = first + i;
    uint16_t on = (ch * PHASE_STEP_TICKS) & 0x0FFF;
    if (ticks[i] == 0) {
      wire_.write(0x00);
      wire_.write(0x00);
      wire_.write(0x00);
      wire_.write(FULL_OFF_BIT);
    } else {
      uint16_t off = (on + ticks[i]) & 0x0FFF;
      wire_.write(on & 0xFF);
      wire_.write(on >> 8);
      wire_.write(off & 0xFF);
      wire_.write(off >> 8);
    }
  }
  bool ok = wire_.endTransmission() == 0;
  ok_ = ok;
  return ok;
}

bool Pca9685::allOff() {
  wire_.beginTransmission(addr_);
  wire_.write(REG_ALL_OFF_L);
  wire_.write(0x00);
  wire_.write(FULL_OFF_BIT);  // REG_ALL_OFF_H (Auto-Increment)
  return wire_.endTransmission() == 0;
}

bool Pca9685::writeReg(uint8_t reg, uint8_t val) {
  wire_.beginTransmission(addr_);
  wire_.write(reg);
  wire_.write(val);
  return wire_.endTransmission() == 0;
}

bool Pca9685::readReg(uint8_t reg, uint8_t& val) {
  wire_.beginTransmission(addr_);
  wire_.write(reg);
  if (wire_.endTransmission(false) != 0) return false;
  if (wire_.requestFrom(addr_, (uint8_t)1) != 1) return false;
  val = wire_.read();
  return true;
}
