#include "pad_control.h"

#include <Preferences.h>

#include "input/pad_ble.h"
#include "input/pad_map.h"
#include "motion/motion.h"
#include "servo/servo_controller.h"
#include "sys/utf8.h"

namespace padctl {
namespace {
constexpr uint32_t PERIOD_MS = 20;
constexpr uint32_t WEB_TIMEOUT_MS = 300;   // Browser sendet ca. alle 50 ms
constexpr uint32_t SAVE_DELAY_MS = 1500;
constexpr float HEIGHT_STEP_MM = 5.0f;

constexpr const char* NVS_NAMESPACE = "hexapod";
constexpr const char* NVS_KEY = "pad";
constexpr uint32_t BLOB_MAGIC = 0x50414431;  // "PAD1"
constexpr uint16_t BLOB_VERSION = 2;  // 2: swap_sticks (1 wird weiter gelesen)

struct Blob {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled, invert_y;
  float deadzone, expo, speed;
  char addr[18];
  uint8_t map[pad::NUM_BUTTONS];
  uint8_t swap_sticks;  // ab Version 2 (passt in die Füllbytes von Version 1)
};
static_assert(sizeof(Blob) == 60, "Version-1-Daten (60 Bytes) müssen lesbar bleiben");

const char* const GESTURES[] = {"circle", "nod", "shake", "pushup", "dance"};
const char* const GESTURE_TEXT[] = {"Kreisen", "Ja", "Nein", "Liegestütz", "Tanzen"};

enum class Src : uint8_t { None, Ble, Web };

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
// --- geschützt durch g_mux (Web-Task <-> loop) ---
padmap::Config g_cfg = padmap::defaults();
bool g_cfg_changed = true;
bool g_enabled = false;
char g_addr[18] = "";
pad::State g_web;
uint32_t g_web_ms = 0;
uint32_t g_dirty_ms = 0;
char g_msg[96] = "";
uint32_t g_msg_seq = 0;

// --- nur in loop() (auch die Telemetrie entsteht dort) ---
padmap::Mapper g_map;
Src g_src = Src::None;
pad::State g_in;
int g_gesture = 0;

void markDirty() {  // im kritischen Abschnitt aufrufen
  const uint32_t now = millis();
  g_dirty_ms = now ? now : 1;
}

void message(const String& m) {
  char buf[sizeof(g_msg)];
  utf8::copy(buf, sizeof(buf), m.c_str());
  portENTER_CRITICAL(&g_mux);
  memcpy(g_msg, buf, sizeof(buf));
  ++g_msg_seq;
  portEXIT_CRITICAL(&g_mux);
}

const char* gaitText(const char* name) {
  if (strcmp(name, "tripod") == 0) return "Dreifuß";
  if (strcmp(name, "ripple") == 0) return "Ripple";
  return "Welle";
}

const char* modeText(padmap::Mode m) {
  switch (m) {
    case padmap::Mode::Body: return "Körper";
    case padmap::Mode::Arms: return "Vorderbeine";
    default:                 return "Laufen";
  }
}

void gesture(int i) {
  String err;
  if (motion::gesture(GESTURES[i], err)) message(String("Geste: ") + GESTURE_TEXT[i]);
  else message(err);
}

void run(padmap::Action action, const motion::Activity& a) {
  using padmap::Action;
  String err;
  switch (action) {
    case Action::StandSit:
      if (!servos.torqueRequested()) {
        // erst Kraft auf die Servos (die Beine gehen in die Sitzhaltung), dann aufstehen
        if (servos.setTorque(true, &err)) message("Servos an – zum Aufstehen noch einmal drücken");
        else message(err);
      } else if (a.standing || a.walking) {
        motion::sit();
        message("Setzt sich");
      } else {
        motion::stand();
        message("Steht auf");
      }
      break;
    case Action::Stop:
      motion::stopWalking();
      if (a.gesture) motion::gesture("stop", err);
      break;
    case Action::ArmsToggle:
      if (!motion::setArms(!a.arms, err)) message(err);
      break;
    case Action::Wave:
      if (!motion::wave(err)) message(err);
      break;
    case Action::GaitPrev:
    case Action::GaitNext: {
      const char* name = motion::cycleGait(action == Action::GaitNext ? 1 : -1);
      message(String("Gangart: ") + gaitText(name));
      break;
    }
    case Action::HeightUp:
    case Action::HeightDown: {
      String note;
      if (motion::nudgeHeight(action == Action::HeightUp ? HEIGHT_STEP_MM : -HEIGHT_STEP_MM, note)) message(note);
      break;
    }
    case Action::SpeedUp:
    case Action::SpeedDown:
      message(String("Tempo ") + (int)lroundf(g_map.speed() * 100.0f) + " %");
      break;
    case Action::AliveToggle:
      motion::setAlive(!a.alive);
      message(a.alive ? "Lebendig aus" : "Lebendig an");
      break;
    case Action::LevelToggle:
      motion::setLeveling(!a.leveling);
      message(a.leveling ? "Lageausgleich aus" : "Lageausgleich an");
      break;
    case Action::GestureNext:
      gesture(g_gesture);
      g_gesture = (g_gesture + 1) % 5;
      break;
    case Action::Circle: gesture(0); break;
    case Action::Nod:    gesture(1); break;
    case Action::Shake:  gesture(2); break;
    case Action::Pushup: gesture(3); break;
    case Action::Dance:  gesture(4); break;
    case Action::Torque: {
      const bool on = !servos.torqueRequested();
      if (servos.setTorque(on, &err)) message(on ? "Servos an" : "Servos aus");
      else message(err);
      break;
    }
    case Action::Estop:
      servos.emergencyStop();
      motion::stopWalking();
      padble::rumble(100, 400);
      message("Not-Aus – in der Oberfläche freigeben");
      break;
    case Action::PoseReset:
      motion::resetPose();
      message("Haltung zurückgesetzt");
      break;
    default:  // Moduswechsel: siehe onMode
      break;
  }
}

// Modus Vorderbeine hebt sie (im Stand), andere Modi setzen sie wieder ab
void onMode(padmap::Mode m, const motion::Activity& a) {
  String err;
  padble::rumble(35, 120);
  message(String("Modus: ") + modeText(m));
  if (m == padmap::Mode::Arms && a.standing && !a.arms) {
    if (!motion::setArms(true, err)) message(err);
  } else if (m != padmap::Mode::Arms && a.arms) {
    motion::setArms(false, err);
  }
}

void apply(const padmap::Output& o) {
  const motion::Activity a = motion::activity();
  if (a.pose) {  // im manuellen Modus (Kalibrierung) bewegen die Sticks nichts
    if (o.walk_send) motion::walk(o.walk[0], o.walk[1], o.walk[2]);
    if (o.body_send) motion::body(o.body[0], o.body[1], o.body[2], o.body[3], o.body[4]);
    if (o.arm_send) motion::armControl(o.arm[0], o.arm[1], o.arm[2], o.arm[3], o.arm[4]);
  }
  for (int i = 0; i < o.n_actions; ++i) run(o.actions[i], a);
  if (o.mode_changed) onMode(o.mode, a);
}

void load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  Blob b;
  size_t len = prefs.getBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
  if (len != sizeof(b) || b.magic != BLOB_MAGIC || b.version < 1 || b.version > BLOB_VERSION) return;
  padmap::Config c = padmap::defaults();
  c.deadzone = isfinite(b.deadzone) ? b.deadzone : c.deadzone;
  c.expo = isfinite(b.expo) ? b.expo : c.expo;
  c.speed = isfinite(b.speed) ? b.speed : c.speed;
  c.invert_y = b.invert_y != 0;
  c.swap_sticks = b.version >= 2 && b.swap_sticks != 0;
  for (int i = 0; i < pad::NUM_BUTTONS; ++i) {
    if (b.map[i] < padmap::NUM_ACTIONS) c.map[i] = static_cast<padmap::Action>(b.map[i]);
  }
  g_cfg = c;
  g_enabled = b.enabled != 0;
  b.addr[sizeof(b.addr) - 1] = 0;
  memcpy(g_addr, b.addr, sizeof(g_addr));
}

void save() {
  Blob b = {};
  portENTER_CRITICAL(&g_mux);
  const padmap::Config c = g_cfg;
  b.enabled = g_enabled;
  memcpy(b.addr, g_addr, sizeof(b.addr));
  portEXIT_CRITICAL(&g_mux);
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.invert_y = c.invert_y;
  b.swap_sticks = c.swap_sticks;
  b.deadzone = c.deadzone;
  b.expo = c.expo;
  b.speed = c.speed;
  for (int i = 0; i < pad::NUM_BUTTONS; ++i) b.map[i] = static_cast<uint8_t>(c.map[i]);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
}

float round2(float v) { return roundf(v * 100.0f) / 100.0f; }
}  // namespace

void begin() {
  load();
  padble::begin(g_enabled, g_addr);
}

void loop() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < PERIOD_MS) return;
  float dt = (now - last) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  last = now;

  portENTER_CRITICAL(&g_mux);
  const bool cfg_changed = g_cfg_changed;
  g_cfg_changed = false;
  const padmap::Config cfg = g_cfg;
  const bool web_fresh = g_web_ms && now - g_web_ms <= WEB_TIMEOUT_MS;
  const pad::State web = g_web;
  const bool save_now = g_dirty_ms && now - g_dirty_ms >= SAVE_DELAY_MS;
  if (save_now) g_dirty_ms = 0;
  portEXIT_CRITICAL(&g_mux);
  if (cfg_changed) g_map.setConfig(cfg);
  if (save_now) save();

  char addr[18];
  if (padble::takePairedAddress(addr)) {
    portENTER_CRITICAL(&g_mux);
    memcpy(g_addr, addr, sizeof(g_addr));
    markDirty();
    portEXIT_CRITICAL(&g_mux);
    message("Controller gekoppelt");
  }

  pad::State in;
  Src src = Src::None;
  if (padble::latest(in)) {
    src = Src::Ble;
  } else if (web_fresh) {
    in = web;
    src = Src::Web;
  }
  if (src != g_src) {
    if (g_src != Src::None) apply(g_map.update(pad::State(), dt));  // alles loslassen
    if (src == Src::Ble) message("Controller verbunden");
    else if (g_src == Src::Ble) message("Controller getrennt");
    g_map.reset(src != Src::None ? in.buttons : 0);
    g_src = src;
    if (src == Src::Ble) padble::rumble(35, 150);
  }
  g_in = in;
  if (src == Src::None) return;
  apply(g_map.update(in, dt));
}

void webInput(const pad::State& s) {
  portENTER_CRITICAL(&g_mux);
  g_web = s;
  const uint32_t now = millis();
  g_web_ms = now ? now : 1;
  portEXIT_CRITICAL(&g_mux);
}

void configToJson(JsonObject obj) {
  portENTER_CRITICAL(&g_mux);
  const padmap::Config c = g_cfg;
  const bool enabled = g_enabled;
  char addr[18];
  memcpy(addr, g_addr, sizeof(addr));
  portEXIT_CRITICAL(&g_mux);
  obj["enabled"] = enabled;
  obj["addr"] = addr;
  obj["deadzone"] = round2(c.deadzone);
  obj["expo"] = round2(c.expo);
  obj["speed"] = round2(c.speed);
  obj["invert_y"] = c.invert_y;
  obj["swap_sticks"] = c.swap_sticks;
  JsonObject map = obj["map"].to<JsonObject>();
  JsonArray buttons = obj["buttons"].to<JsonArray>();
  for (int i = 0; i < pad::NUM_BUTTONS; ++i) {
    map[pad::buttonName(i)] = padmap::actionName(c.map[i]);
    buttons.add(pad::buttonName(i));
  }
  JsonArray actions = obj["actions"].to<JsonArray>();
  for (int i = 0; i < padmap::NUM_ACTIONS; ++i) actions.add(padmap::actionName(static_cast<padmap::Action>(i)));
}

bool applyJson(JsonVariantConst in, String& err) {
  portENTER_CRITICAL(&g_mux);
  padmap::Config c = g_cfg;
  bool enabled = g_enabled;
  portEXIT_CRITICAL(&g_mux);

  if (in["reset"] | false) c = padmap::defaults();
  if (in["deadzone"].is<float>()) c.deadzone = constrain(in["deadzone"].as<float>(), 0.02f, 0.4f);
  if (in["expo"].is<float>()) c.expo = constrain(in["expo"].as<float>(), 0.0f, 1.0f);
  if (in["speed"].is<float>()) c.speed = constrain(in["speed"].as<float>(), 0.1f, 1.0f);
  if (in["invert_y"].is<bool>()) c.invert_y = in["invert_y"];
  if (in["swap_sticks"].is<bool>()) c.swap_sticks = in["swap_sticks"];
  if (in["map"].is<JsonObjectConst>()) {
    for (JsonPairConst kv : in["map"].as<JsonObjectConst>()) {
      const int b = pad::buttonFromName(kv.key().c_str());
      padmap::Action a;
      if (b < 0 || !padmap::actionFromName(kv.value().as<const char*>(), a)) {
        err = String("unbekannte Taste oder Aktion: ") + kv.key().c_str();
        return false;
      }
      c.map[b] = a;
    }
  }
  if (in["enabled"].is<bool>()) enabled = in["enabled"];
  const bool pair = in["pair"] | false;
  const bool forget = in["forget"] | false;
  if (pair) enabled = true;  // Koppeln schaltet Bluetooth ein

  portENTER_CRITICAL(&g_mux);
  g_cfg = c;
  g_cfg_changed = true;
  g_enabled = enabled;
  if (forget) g_addr[0] = 0;
  markDirty();
  portEXIT_CRITICAL(&g_mux);
  padble::setEnabled(enabled);
  if (forget) padble::forget();
  if (pair) padble::startPairing();
  return true;
}

void telemetry(JsonObject obj) {
  static const char* const SRC[] = {"", "ble", "web"};
  obj["src"] = SRC[static_cast<int>(g_src)];
  obj["mode"] = padmap::modeName(g_map.mode());
  obj["speed"] = (int)lroundf(g_map.speed() * 100.0f);
  const padble::Status b = padble::status();
  obj["ble"] = padble::stateName(b.state);
  // Achtung ArduinoJson 7: ein const-char-Array (b ist const) gilt als String-Literal und wird
  // nur als Zeiger gespeichert – b existiert beim Serialisieren nicht mehr (Datenmüll im JSON,
  // der Browser trennt die WebSocket-Verbindung). Als Zeiger übergeben wird kopiert.
  const char* name = b.name;
  if (name[0]) obj["name"] = name;
  if (b.battery >= 0) obj["bat"] = b.battery;
  if (b.pairing_left_s) obj["pair_left"] = b.pairing_left_s;
  if (g_src != Src::None) {
    JsonObject i = obj["in"].to<JsonObject>();
    i["lx"] = round2(g_in.lx);
    i["ly"] = round2(g_in.ly);
    i["rx"] = round2(g_in.rx);
    i["ry"] = round2(g_in.ry);
    i["lt"] = round2(g_in.lt);
    i["rt"] = round2(g_in.rt);
    i["b"] = g_in.buttons;
  }
  portENTER_CRITICAL(&g_mux);
  const uint32_t seq = g_msg_seq;
  char msg[sizeof(g_msg)];
  memcpy(msg, g_msg, sizeof(msg));
  portEXIT_CRITICAL(&g_mux);
  if (seq) {
    obj["msg"] = msg;
    obj["seq"] = seq;
  }
}

}  // namespace padctl
