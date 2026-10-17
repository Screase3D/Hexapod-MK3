#pragma once
// Augen-LEDs: Zustände, Farben, Effekte – reine Logik ohne Hardware (Tests:
// test_host/led_test.cpp). StatusLeds wählt den Zustand, rechnet hier die Farbe
// beider Augen aus und schreibt sie auf die WS2812B.

#include <stdint.h>

namespace ledfx {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
};
bool operator==(const Rgb& a, const Rgb& b);

// Neue Effekte nur hinten anhängen (Nummern stehen im NVS)
enum class Effect : uint8_t { Off, Solid, Breathe, Blink, Pulse, Wink, Alternate, Rainbow, Flicker, Strobe, Scan, Sleepy,
                              BlinkFast };
constexpr int NUM_EFFECTS = 13;
// "off", "solid", "breathe", "blink", "pulse", "wink", "alternate", "rainbow",
// "flicker" (unruhig, je Auge eigen), "strobe" (schnelles Blitzen), "scan" (Licht wandert hin und her),
// "sleepy" (gedimmt, langsames Lidschließen), "blink_fast" (schnell blinken, 4 Hz)
const char* effectName(Effect e);
bool effectFromName(const char* name, Effect& out);

// Zustände, nach Vorrang sortiert wählt sie StatusLeds. Einstellbar sind ap ... gesture;
// Start, Verbinden und Warnungen sind fest (immer erkennbar).
enum class State : uint8_t {
  Boot,         // Start
  WifiConnect,  // verbindet mit dem WLAN
  Ap,           // Access Point, Servos aus
  Idle,         // Servos aus
  Torque,       // Servos an, sitzt
  Stand,        // steht
  Walk,         // läuft
  Arms,         // Vorderbeine oben
  Gesture,      // Geste
  BatteryWarn,  // Akku schwach
  BatteryLow,   // Akku leer
  Estop,        // Not-Aus
};
constexpr int NUM_STATES = 12;
const char* stateName(State s);  // "boot", "wifi", "ap", "idle", "torque", "stand", "walk", "arms", "gesture",
                                 // "bat_warn", "bat_low", "estop"
bool stateFromName(const char* name, State& out);
bool configurable(State s);

struct Style {
  Rgb color;
  Effect effect = Effect::Solid;
};

enum class Mode : uint8_t { Auto, Single, Off };  // je Zustand / eine Farbe / aus
const char* modeName(Mode m);                     // "auto", "single", "off"
bool modeFromName(const char* name, Mode& out);

enum class Board : uint8_t { Boot, Mirror, Off };  // Board-LED: nur beim Start / wie die Augen / aus
const char* boardName(Board b);                    // "boot", "mirror", "off"
bool boardFromName(const char* name, Board& out);

struct Config {
  Mode mode = Mode::Auto;
  uint8_t brightness = 100;  // % der zulässigen Helligkeit
  Board board = Board::Boot;
  Style single;
  Style styles[NUM_STATES];  // nur die einstellbaren werden benutzt
};
Config defaults();
Style fixedStyle(State s);  // für nicht einstellbare Zustände

// Stil, der im Zustand s gilt (Warnungen immer fest, auch bei "aus")
Style styleFor(const Config& c, State s);

// Farbe beider Augen (0 = links, 1 = rechts) zum Zeitpunkt t (ms), volle Helligkeit
void render(const Style& s, uint32_t t_ms, Rgb out[2]);

Rgb scale(const Rgb& c, float k);
bool parseHex(const char* s, Rgb& out);  // "#rrggbb"
void toHex(const Rgb& c, char out[8]);

}  // namespace ledfx
