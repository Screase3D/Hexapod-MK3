#include "status_leds.h"

#include <Preferences.h>
#include <math.h>

#include "config.h"

StatusLeds statusLeds;

namespace {
constexpr int EYE_COUNT = 2;
constexpr uint32_t RMT_TICK_HZ = 10000000;  // 100 ns pro Tick
constexpr uint32_t PREVIEW_MS = 2500;       // Vorschau beim Einstellen
constexpr uint32_t SAVE_DELAY_MS = 1500;

// WS2812B-Bit-Timing in 100-ns-Ticks
constexpr uint16_t T1H = 8, T1L = 4, T0H = 4, T0L = 8;

constexpr const char* NVS_NAMESPACE = "hexapod";
constexpr const char* NVS_KEY = "leds";
constexpr uint32_t BLOB_MAGIC = 0x4C454431;  // "LED1"
constexpr uint16_t BLOB_VERSION = 1;

struct Blob {
  uint32_t magic;
  uint16_t version;
  uint8_t mode, brightness, board;
  uint8_t single[4];                       // r, g, b, Effekt
  uint8_t styles[ledfx::NUM_STATES][4];
};

void pack(const ledfx::Style& s, uint8_t out[4]) {
  out[0] = s.color.r;
  out[1] = s.color.g;
  out[2] = s.color.b;
  out[3] = static_cast<uint8_t>(s.effect);
}

bool unpack(const uint8_t in[4], ledfx::Style& s) {
  if (in[3] >= ledfx::NUM_EFFECTS) return false;
  s.color = {in[0], in[1], in[2]};
  s.effect = static_cast<ledfx::Effect>(in[3]);
  return true;
}

void styleToJson(const ledfx::Style& s, JsonObject o) {
  char hex[8];
  ledfx::toHex(s.color, hex);
  o["color"] = hex;
  o["effect"] = ledfx::effectName(s.effect);
}

// "color"/"effect" aus in übernehmen (einzeln oder beide)
bool styleFromJson(JsonVariantConst in, ledfx::Style& s, String& err) {
  if (in["color"].is<const char*>() && !ledfx::parseHex(in["color"], s.color)) {
    err = "color muss #rrggbb sein";
    return false;
  }
  if (in["effect"].is<const char*>() && !ledfx::effectFromName(in["effect"], s.effect)) {
    err = "unbekannter Effekt";
    return false;
  }
  return true;
}
}  // namespace

void StatusLeds::begin() {
  load();
  ok_ = rmtInit(PIN_EYES_LED, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_TICK_HZ);
  if (!ok_) log_e("RMT für Augen-LEDs (GPIO %d) fehlgeschlagen", PIN_EYES_LED);
  update();
}

void StatusLeds::off() {
  ledfx::Rgb dark[2];
  write(dark, ledfx::Rgb());
}

void StatusLeds::write(const ledfx::Rgb eyes[2], const ledfx::Rgb& board) {
  if (!(board == last_board_) || !written_) {
    last_board_ = board;
    rgbLedWrite(PIN_BOARD_LED, board.r, board.g, board.b);  // eine WS2812, Core-Funktion
  }
  if (written_ && eyes[0] == last_[0] && eyes[1] == last_[1]) return;
  last_[0] = eyes[0];
  last_[1] = eyes[1];
  written_ = true;
  if (!ok_) return;
  rmt_data_t data[EYE_COUNT * 24];
  int i = 0;
  for (int led = 0; led < EYE_COUNT; ++led) {
    const uint8_t grb[3] = {eyes[led].g, eyes[led].r, eyes[led].b};  // WS2812B erwartet G-R-B
    for (uint8_t c : grb) {
      for (int bit = 7; bit >= 0; --bit) {
        bool one = c & (1 << bit);
        data[i].level0 = 1;
        data[i].duration0 = one ? T1H : T0H;
        data[i].level1 = 0;
        data[i].duration1 = one ? T1L : T0L;
        ++i;
      }
    }
  }
  rmtWrite(PIN_EYES_LED, data, RMT_SYMBOLS_OF(data), 10);
}

void StatusLeds::update() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  const ledfx::Config cfg = cfg_;
  const bool preview = preview_until_ && (int32_t)(preview_until_ - now) > 0;
  const ledfx::Style pv = preview_;
  const Life life = life_;
  if (!preview) preview_until_ = 0;
  const bool save_now = dirty_ms_ && now - dirty_ms_ >= SAVE_DELAY_MS;
  if (save_now) dirty_ms_ = 0;
  portEXIT_CRITICAL(&mux_);
  if (save_now) save();

  const ledfx::State s = state_;
  // Vorschau/Vorgabe und "Lebendig" nicht über Warnungen (Not-Aus, Akku) legen
  const bool own = ledfx::configurable(s);
  const bool life_style = own && !preview && life.style_on;
  const ledfx::Style style = preview && own ? pv : (life_style ? life.style : ledfx::styleFor(cfg, s));
  ledfx::Rgb eyes[2];
  ledfx::render(style, now, eyes);
  float k[2];
  k[0] = k[1] = cfg.brightness / 100.0f * LED_MAX_BRIGHTNESS / 255.0f;
  if (own && !preview && !life_style) {
    // atmen, gelangweilt dimmen, in die Bewegungsrichtung schauen (Auge 0 = links)
    const float b = (1.0f - life.breath) * life.dim;
    k[0] *= b * (life.gaze < 0.0f ? 1.0f + 0.75f * life.gaze : 1.0f);
    k[1] *= b * (life.gaze > 0.0f ? 1.0f - 0.75f * life.gaze : 1.0f);
  }
  eyes[0] = ledfx::scale(eyes[0], k[0]);
  eyes[1] = ledfx::scale(eyes[1], k[1]);

  // Board-LED (vor der Kamera): standardmäßig nur, solange das Netz noch nicht steht
  ledfx::Rgb board;
  const bool booting = s == ledfx::State::Boot || s == ledfx::State::WifiConnect;
  if (cfg.board == ledfx::Board::Mirror || (cfg.board == ledfx::Board::Boot && booting)) board = eyes[0];
  write(eyes, board);
}

void StatusLeds::showOverride(const ledfx::Style& st, uint32_t ms) {
  portENTER_CRITICAL(&mux_);
  preview_ = st;
  const uint32_t until = millis() + ms;
  preview_until_ = ms ? (until ? until : 1) : 0;
  portEXIT_CRITICAL(&mux_);
}

void StatusLeds::setLife(const Life& l) {
  portENTER_CRITICAL(&mux_);
  life_ = l;
  portEXIT_CRITICAL(&mux_);
}

bool StatusLeds::overrideStyle(ledfx::Style& out) {
  portENTER_CRITICAL(&mux_);
  const bool on = preview_until_ && (int32_t)(preview_until_ - millis()) > 0;
  out = preview_;
  portEXIT_CRITICAL(&mux_);
  return on && ledfx::configurable(state_);
}

void StatusLeds::configToJson(JsonObject obj) {
  portENTER_CRITICAL(&mux_);
  const ledfx::Config c = cfg_;
  portEXIT_CRITICAL(&mux_);
  obj["mode"] = ledfx::modeName(c.mode);
  obj["brightness"] = c.brightness;
  obj["board"] = ledfx::boardName(c.board);
  styleToJson(c.single, obj["single"].to<JsonObject>());
  JsonArray st = obj["states"].to<JsonArray>();
  JsonArray fx = obj["fixed"].to<JsonArray>();
  for (int i = 0; i < ledfx::NUM_STATES; ++i) {
    const ledfx::State s = static_cast<ledfx::State>(i);
    // einstellbare: eigene Einstellung (auch bei "eine Farbe"/"aus"), sonst die feste
    const bool own = ledfx::configurable(s);
    JsonObject o = own ? st.add<JsonObject>() : fx.add<JsonObject>();
    o["id"] = ledfx::stateName(s);
    styleToJson(own ? c.styles[i] : ledfx::fixedStyle(s), o);
  }
  obj["state"] = ledfx::stateName(state_);
}

bool StatusLeds::applyJson(JsonVariantConst in, String& err) {
  portENTER_CRITICAL(&mux_);
  ledfx::Config c = cfg_;
  portEXIT_CRITICAL(&mux_);
  ledfx::Style shown;
  bool show = false;

  if (in["reset"] | false) c = ledfx::defaults();
  if (in["mode"].is<const char*>() && !ledfx::modeFromName(in["mode"], c.mode)) {
    err = "mode muss auto, single oder off sein";
    return false;
  }
  if (in["brightness"].is<int>()) c.brightness = constrain(in["brightness"].as<int>(), 5, 100);
  if (in["board"].is<const char*>() && !ledfx::boardFromName(in["board"], c.board)) {
    err = "board muss boot, mirror oder off sein";
    return false;
  }
  if (in["single"].is<JsonObjectConst>()) {
    if (!styleFromJson(in["single"], c.single, err)) return false;
    shown = c.single;
    show = true;
  }
  if (in["state"].is<const char*>()) {
    ledfx::State s;
    if (!ledfx::stateFromName(in["state"], s) || !ledfx::configurable(s)) {
      err = "Zustand nicht einstellbar";
      return false;
    }
    ledfx::Style& st = c.styles[static_cast<int>(s)];
    if (!styleFromJson(in, st, err)) return false;
    shown = st;
    show = true;
  }
  const bool preview = (in["preview"] | false) && show;

  portENTER_CRITICAL(&mux_);
  cfg_ = c;
  uint32_t now = millis();
  dirty_ms_ = now ? now : 1;
  if (preview) {
    preview_ = shown;
    preview_until_ = now + PREVIEW_MS;
    if (!preview_until_) preview_until_ = 1;
  }
  portEXIT_CRITICAL(&mux_);
  return true;
}

void StatusLeds::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  Blob b;
  size_t len = prefs.getBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
  if (len != sizeof(b) || b.magic != BLOB_MAGIC || b.version != BLOB_VERSION) return;
  ledfx::Config c = ledfx::defaults();
  if (b.mode > static_cast<uint8_t>(ledfx::Mode::Off) || b.board > static_cast<uint8_t>(ledfx::Board::Off)) return;
  c.mode = static_cast<ledfx::Mode>(b.mode);
  c.board = static_cast<ledfx::Board>(b.board);
  c.brightness = constrain(b.brightness, 5, 100);
  if (!unpack(b.single, c.single)) return;
  for (int i = 0; i < ledfx::NUM_STATES; ++i) {
    if (ledfx::configurable(static_cast<ledfx::State>(i)) && !unpack(b.styles[i], c.styles[i])) return;
  }
  cfg_ = c;
}

void StatusLeds::save() {
  portENTER_CRITICAL(&mux_);
  const ledfx::Config c = cfg_;
  portEXIT_CRITICAL(&mux_);
  Blob b = {};
  b.magic = BLOB_MAGIC;
  b.version = BLOB_VERSION;
  b.mode = static_cast<uint8_t>(c.mode);
  b.brightness = c.brightness;
  b.board = static_cast<uint8_t>(c.board);
  pack(c.single, b.single);
  for (int i = 0; i < ledfx::NUM_STATES; ++i) pack(c.styles[i], b.styles[i]);
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes(NVS_KEY, &b, sizeof(b));
  prefs.end();
}
