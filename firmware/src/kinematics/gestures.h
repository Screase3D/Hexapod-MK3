#pragma once
// Gesten: kurze Körperbewegungen im Stand – reine Mathematik (Tests:
// test_host/moves_test.cpp). Die Füße bleiben stehen; die Geste liefert einen
// Versatz, der zur eingestellten Körperhaltung addiert wird. Ein- und Ausblenden
// ohne Ruck (Hüllkurve), Abbruch blendet weich aus.

#include <stdint.h>

namespace gestures {

enum class Type : uint8_t { None, Circle, Nod, Shake, Pushup, Dance };
const char* typeName(Type t);  // "none", "circle", "nod", "shake", "pushup", "dance"
bool typeFromName(const char* name, Type& out);

struct Offset {
  float x = 0, y = 0, z = 0;              // mm
  float roll = 0, pitch = 0, yaw = 0;     // Grad
};

class Player {
 public:
  void start(Type t);       // ersetzt eine laufende Geste
  void stop();              // blendet aus
  void reset();             // sofort aus
  Offset update(float dt);

  Type active() const { return type_; }
  float duration() const;   // Gesamtdauer der laufenden Geste (s)

 private:
  Type type_ = Type::None;
  float t_ = 0.0f;
  float stop_at_ = -1.0f;   // Zeitpunkt des Ausblendens bei Abbruch
};

}  // namespace gestures
