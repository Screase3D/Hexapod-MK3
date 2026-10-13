#pragma once
// Gamepad -> Bewegungsbefehle: Totzone und Kennlinie der Sticks, Modi (Laufen,
// Körper, Vorderbeine), Tastenbelegung mit Flankenerkennung und Wiederholung beim
// Halten. Reine Logik (Tests: test_host/pad_test.cpp); die Ausführung der Aktionen
// übernimmt pad_control.cpp.

#include <stdint.h>

#include "pad.h"

namespace padmap {

enum class Action : uint8_t {
  None, StandSit, Stop, ArmsToggle, Wave, ModeNext, ModeWalk, ModeBody, ModeArms,
  GaitPrev, GaitNext, HeightUp, HeightDown, SpeedUp, SpeedDown, AliveToggle, LevelToggle,
  GestureNext, Circle, Nod, Shake, Pushup, Dance, Torque, Estop, PoseReset,
};
constexpr int NUM_ACTIONS = 26;
const char* actionName(Action a);  // "none", "stand_sit", "stop", "arms", "wave", "mode_next", ...
bool actionFromName(const char* name, Action& out);

enum class Mode : uint8_t { Walk, Body, Arms };
const char* modeName(Mode m);  // "walk", "body", "arms"

struct Config {
  float deadzone = 0.12f;  // Sticks: so viel Ausschlag wird ignoriert (0.02..0.4)
  float expo = 0.35f;      // Kennlinie: 0 = linear, 1 = sehr fein um die Mitte
  float speed = 0.6f;      // Grundtempo beim Laufen; RT gibt bis 100 %, LT bremst
  bool invert_y = false;   // Sticks oben/unten vertauscht
  bool swap_sticks = false;  // linker und rechter Stick getauscht (z. B. Linkshänder)
  Action map[pad::NUM_BUTTONS] = {};
};
Config defaults();

struct Output {
  Mode mode = Mode::Walk;
  bool mode_changed = false;
  // Dauerbefehle: senden, solange aktiv, und einmal null nach dem Loslassen
  bool walk_send = false;
  float walk[3] = {0, 0, 0};          // vx, vy, turn (-1..1)
  bool body_send = false;
  float body[5] = {0, 0, 0, 0, 0};    // x, y, roll, pitch, yaw
  bool arm_send = false;
  float arm[5] = {0, 0, 0, 0, 0};     // lx, lz, rx, rz, reach
  // ausgelöste Aktionen (Druck bzw. Wiederholung beim Halten)
  static constexpr int MAX_ACTIONS = 8;
  Action actions[MAX_ACTIONS];
  int n_actions = 0;
};

class Mapper {
 public:
  Mapper() : cfg_(defaults()), speed_(cfg_.speed) {}
  void setConfig(const Config& c);
  const Config& config() const { return cfg_; }
  // Controller neu verbunden/getrennt. held: schon gedrückte Tasten lösen nichts aus
  // (z. B. die Xbox-Taste, mit der der Controller eingeschaltet wurde)
  void reset(uint32_t held = 0);
  Output update(const pad::State& in, float dt);

  Mode mode() const { return mode_; }
  float speed() const { return speed_; }

 private:
  void stick(float x, float y, float& ox, float& oy) const;  // Totzone (rund) + Kennlinie
  float axis(float v) const;                                  // Trigger: Totzone + Kennlinie

  Config cfg_;
  Mode mode_ = Mode::Walk;
  float speed_;
  uint32_t prev_ = 0;
  float held_[pad::NUM_BUTTONS] = {};  // wie lange gehalten (s)
  float next_repeat_[pad::NUM_BUTTONS] = {};
  bool walk_was_ = false, body_was_ = false, arm_was_ = false;
};

}  // namespace padmap
