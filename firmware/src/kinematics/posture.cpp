#include "posture.h"

#include "gait.h"

namespace posture {
namespace {
// Mindestdauer der Schritt-Phasen: die Gangart braucht einen Regelschritt,
// um die neuen Neutralpunkte zu bemerken und loszulaufen
constexpr float STEP_PHASE_MIN_S = 0.1f;
}  // namespace

const char* stateName(State s) {
  switch (s) {
    case State::StandingUp:  return "standing_up";
    case State::Standing:    return "standing";
    case State::SittingDown: return "sitting_down";
    default:                 return "sitting";
  }
}

void Sequencer::reset(State s) {
  state_ = s;
  phase_ = 0;
  t_ = 0;
  latch_ = s == State::StandingUp;
}

bool Sequencer::requestStand() {
  if (state_ == State::SittingDown) return false;
  if (state_ == State::Sitting) {
    state_ = State::StandingUp;
    phase_ = 0;
    t_ = 0;
    latch_ = true;
  }
  return true;
}

bool Sequencer::requestSit() {
  if (state_ == State::StandingUp) return false;
  if (state_ == State::Standing) {
    state_ = State::SittingDown;
    phase_ = 0;
    t_ = 0;
  }
  return true;
}

Output Sequencer::update(float dt, float stand_height, float stand_stance, float current_height,
                         bool gait_idle) {
  t_ += dt;
  switch (state_) {
    case State::Sitting:
      return {true, cfg_.sit_height, cfg_.sit_stance};

    case State::StandingUp:
      if (latch_) {
        latch_ = false;
        h1_ = stand_height;
        s1_ = stand_stance;
      }
      if (phase_ == 0) {
        // 1. anheben, Füße bleiben weit außen stehen
        float s = gait::smoothStep5(t_ / cfg_.rise_s);
        float h = cfg_.sit_height + (h1_ - cfg_.sit_height) * s;
        if (t_ >= cfg_.rise_s) {
          phase_ = 1;
          t_ = 0;
        }
        return {true, h, cfg_.sit_stance};
      }
      // 2. Füße auf den normalen Fußabstand setzen (Schritte der Gangart)
      if (t_ >= STEP_PHASE_MIN_S && gait_idle) {
        state_ = State::Standing;
        return {false, stand_height, stand_stance};
      }
      return {true, h1_, s1_};

    case State::SittingDown:
      if (phase_ == 0) {
        // 1. Füße nach außen setzen (Höhe halten), Körper gerade ausrichten
        if (t_ <= dt) h0_ = current_height;
        if (t_ >= STEP_PHASE_MIN_S && gait_idle) {
          phase_ = 1;
          t_ = 0;
          h0_ = current_height;
        }
        return {true, h0_, cfg_.sit_stance};
      } else {
        // 2. absenken
        float s = gait::smoothStep5(t_ / cfg_.rise_s);
        if (t_ >= cfg_.rise_s) {
          state_ = State::Sitting;
          return {true, cfg_.sit_height, cfg_.sit_stance};
        }
        return {true, h0_ + (cfg_.sit_height - h0_) * s, cfg_.sit_stance};
      }

    default:  // Standing
      return {false, stand_height, stand_stance};
  }
}

}  // namespace posture
