#pragma once
// Servokalibrierung: Abbildung Gelenkwinkel (Grad) <-> Pulsbreite (µs).
//
//   puls_us = center_us + winkel_deg * us_per_deg
//
// center_us   Puls in der Neutralstellung (Gelenkwinkel 0°, siehe docs/kalibrierung.md)
// us_per_deg  Steigung inkl. Vorzeichen (Vorzeichen = Drehrichtung)
// min/max_deg Software-Endanschläge des Gelenks

#include <ArduinoJson.h>
#include "config.h"

struct ServoCal {
  float center_us;
  float us_per_deg;
  float min_deg;
  float max_deg;
};

class Calibration {
 public:
  Calibration() { setDefaults(); }

  void setDefaults();
  static ServoCal defaultFor(int id);

  bool load();  // aus NVS; false -> Standardwerte aktiv
  bool save() const;

  ServoCal& operator[](int id) { return cal_[id]; }
  const ServoCal& operator[](int id) const { return cal_[id]; }

  // Winkel wird auf [min_deg, max_deg] begrenzt
  float degToUs(int id, float deg) const;
  float usToDeg(int id, float us) const;
  float clampDeg(int id, float deg) const;

  // Plausibilitätsprüfung eines Datensatzes; err enthält ggf. den Grund
  static bool validate(const ServoCal& c, String& err);

  void toJson(JsonObject obj, int id) const;
  void toJson(JsonDocument& doc) const;
  // Übernimmt alle Servos aus JSON (Format wie toJson). Bei Fehlern bleibt
  // die bisherige Kalibrierung unverändert.
  bool fromJson(JsonVariantConst root, String& err);
  static bool entryFromJson(JsonVariantConst obj, ServoCal& out, String& err);

 private:
  ServoCal cal_[NUM_SERVOS];
};
