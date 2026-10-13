#include "pad.h"

#include <cstring>

namespace pad {
namespace {
const char* const NAMES[NUM_BUTTONS] = {"a",  "b",  "x",    "y",    "lb",   "rb",    "view",
                                        "menu", "xbox", "ls", "rs", "up",   "down",  "left",
                                        "right", "share", "p1", "p2", "p3", "p4"};

uint16_t u16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | d[1] << 8); }

float stick(uint16_t v) {  // 0..65535, Mitte 32768 -> -1..1
  float f = (static_cast<float>(v) - 32768.0f) / 32767.0f;
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

float trigger(uint16_t v) {  // 0..1023
  float f = v / 1023.0f;
  return f > 1.0f ? 1.0f : f;
}
}  // namespace

const char* buttonName(int b) { return b >= 0 && b < NUM_BUTTONS ? NAMES[b] : "?"; }

int buttonFromName(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < NUM_BUTTONS; ++i) {
    if (std::strcmp(name, NAMES[i]) == 0) return i;
  }
  return -1;
}

bool parseXboxReport(const uint8_t* d, size_t len, State& out) {
  if (len < 16) return false;
  State s;
  s.lx = stick(u16(d + 0));
  s.ly = -stick(u16(d + 2));  // HID: oben = 0
  s.rx = stick(u16(d + 4));
  s.ry = -stick(u16(d + 6));
  s.lt = trigger(u16(d + 8));
  s.rt = trigger(u16(d + 10));

  uint32_t b = 0;
  const uint8_t hat = d[12];  // 0 = nichts, 1 = oben, dann im Uhrzeigersinn bis 8
  if (hat == 1 || hat == 2 || hat == 8) b |= mask(BTN_UP);
  if (hat >= 2 && hat <= 4) b |= mask(BTN_RIGHT);
  if (hat >= 4 && hat <= 6) b |= mask(BTN_DOWN);
  if (hat >= 6 && hat <= 8) b |= mask(BTN_LEFT);
  const uint8_t m = d[13];
  if (m & 0x01) b |= mask(BTN_A);
  if (m & 0x02) b |= mask(BTN_B);
  if (m & 0x08) b |= mask(BTN_X);
  if (m & 0x10) b |= mask(BTN_Y);
  if (m & 0x40) b |= mask(BTN_LB);
  if (m & 0x80) b |= mask(BTN_RB);
  const uint8_t c = d[14];
  if (c & 0x04) b |= mask(BTN_VIEW);
  if (c & 0x08) b |= mask(BTN_MENU);
  if (c & 0x10) b |= mask(BTN_XBOX);
  if (c & 0x20) b |= mask(BTN_LS);
  if (c & 0x40) b |= mask(BTN_RS);
  if (d[15] & 0x01) b |= mask(BTN_SHARE);
  if (len > 18) {  // Elite 2: Paddles (hinten)
    const uint8_t p = d[18];
    if (p & 0x01) b |= mask(BTN_P1);
    if (p & 0x02) b |= mask(BTN_P2);
    if (p & 0x04) b |= mask(BTN_P3);
    if (p & 0x08) b |= mask(BTN_P4);
  }
  s.buttons = b;
  out = s;
  return true;
}

}  // namespace pad
