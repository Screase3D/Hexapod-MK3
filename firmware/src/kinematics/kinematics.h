#pragma once
// Kinematik des Hexapods – reine Mathematik ohne Arduino-Abhängigkeiten,
// damit sie auch auf dem PC getestet werden kann (test_host/).
//
// Koordinatensysteme (Einheiten: mm, Winkel in der Schnittstelle: Grad)
//   Welt:  Ursprung auf dem Boden unter der Körpermitte (Nullpose),
//          x vorne, y links, z oben
//   Körper: Ursprung in der Körpermitte auf Höhe der Coxa-Achsen
//   Bein:  Ursprung in der Coxa-Achse, x zeigt in Montagerichtung (Coxa 0°),
//          y 90° gegen den Uhrzeigersinn dazu, z oben
//
// Gelenkwinkel folgen der Konvention aus docs/kalibrierung.md:
//   Coxa  0° = entlang der Montagerichtung, + = Fuß nach vorne (beide Seiten)
//   Femur 0° = waagerecht, + = nach oben
//   Tibia 0° = 90° zum Femur, + = Fuß nach außen (Knie öffnet sich)

#include <stdint.h>

namespace kin {

struct Vec3 {
  float x, y, z;
};

struct LegGeometry {
  float coxa;   // Coxa-Achse -> Femur-Achse (waagerecht)
  float femur;  // Femur-Achse -> Knie-Achse
  float tibia;  // Knie-Achse -> Fußspitze
};

struct LegMount {
  float x, y;       // Coxa-Achse relativ zur Körpermitte
  float angle_deg;  // Richtung der Coxa bei 0°, gegen den Uhrzeigersinn ab +x
};

struct JointAngles {
  float coxa, femur, tibia;  // Grad
};

struct BodyPose {
  float x, y, z;     // Lage der Körpermitte über dem Welt-Ursprung (z = Höhe)
  float roll_deg;    // + = rechte Seite runter (linke Seite hoch)
  float pitch_deg;   // + = Nase hoch
  float yaw_deg;     // + = nach links drehen (von oben gegen den Uhrzeigersinn)
};

// +1, wenn eine mathematisch positive Coxa-Drehung (gegen den Uhrzeigersinn)
// den Fuß nach vorne bewegt (rechte Seite), sonst -1 (linke Seite).
float coxaSign(const LegMount& m);

// Inverse Kinematik: Fußpunkt im Bein-System -> Gelenkwinkel.
// false = Punkt nicht erreichbar (out bleibt dann unverändert).
bool legIk(const LegGeometry& g, const LegMount& m, const Vec3& foot_leg, JointAngles& out);

// Vorwärtskinematik: Gelenkwinkel -> Fußpunkt im Bein-System.
Vec3 legFk(const LegGeometry& g, const LegMount& m, const JointAngles& a);

// Umrechnung zwischen Körper- und Bein-System
Vec3 bodyToLeg(const LegMount& m, const Vec3& p_body);
Vec3 legToBody(const LegMount& m, const Vec3& p_leg);

// Weltpunkt (z. B. Fuß auf dem Boden) -> Körper-System bei gegebener Körperlage
Vec3 worldToBody(const BodyPose& pose, const Vec3& p_world);

// Standard-Fußpunkt eines Beins in Weltkoordinaten: in Montagerichtung im
// Abstand `stance` von der Coxa-Achse, auf dem Boden (z = 0).
Vec3 stanceFoot(const LegMount& m, float stance);

// Coxa-Achse in Weltkoordinaten (auf den Boden projiziert) bei verschobenem und
// gedrehtem Körper; Rollen/Nicken bleiben unberücksichtigt.
Vec3 coxaWorld(const BodyPose& pose, const LegMount& m);

// Haltungsprüfung: Erreichen alle Beine ihre Fußpunkte feet[] (Weltkoordinaten)
// bei Körperlage pose, und bleiben alle Gelenke in den Grenzen lo/hi (Grad, je
// Gelenk in der Reihenfolge Bein*3 + Gelenk)?
struct PoseCheck {
  uint8_t unreachable = 0;  // Bitmaske Beine: Punkt außer Reichweite
  uint8_t limited = 0;      // Bitmaske Beine: Gelenk außerhalb der Grenzen
  bool ok() const { return unreachable == 0 && limited == 0; }
};
PoseCheck checkPose(const LegGeometry& g, const LegMount* mounts, int legs, const BodyPose& pose,
                    const Vec3* feet, const float* lo, const float* hi);

// Einzelnes Bein: Fußpunkt foot (Welt) bei Körperlage pose erreichbar, alle Gelenke
// mindestens margin_deg innerhalb von lo/hi (je 3 Werte: Coxa, Femur, Tibia) und
// Kniewinkel (90° + Tibia) mindestens knee_min_deg?
bool footReachable(const LegGeometry& g, const LegMount& m, const BodyPose& pose, const Vec3& foot,
                   const float* lo, const float* hi, float margin_deg, float knee_min_deg);

// Haltung aus den Gelenkwinkeln schätzen (Vorwärtskinematik, Füße auf dem Boden
// angenommen): mittlere Höhe der Coxa-Achsen über den Füßen und mittlerer Fußabstand.
// deg: je Bein Coxa, Femur, Tibia. false bei unplausiblen Werten.
bool estimateStance(const LegGeometry& g, const LegMount* mounts, int legs, const float* deg, float& height,
                    float& stance);

// Zusammenhängender zulässiger Bereich um cur innerhalb [lo, hi] (z. B. eine Haltungsgröße,
// die anderen fest): ok(ctx, v) prüft einen Wert. Je Seite erst in steps groben Schritten
// nach außen, dann die Grenze iters-mal halbieren. Ist cur selbst unzulässig: [cur, cur].
void feasibleInterval(bool (*ok)(void*, float), void* ctx, float cur, float lo, float hi, int steps,
                      int iters, float& out_lo, float& out_hi);

}  // namespace kin
