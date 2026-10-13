#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace utf8 {

// Abgeschnittenen Text nicht mitten in einem UTF-8-Zeichen enden lassen (z. B. „ü“ = 2 Bytes).
// Ein halbes Zeichen macht eine WebSocket-Textnachricht ungültig – der Browser trennt dann.
inline void trimIncomplete(char* s) {
  const size_t n = strlen(s);
  size_t i = n;
  while (i > 0 && n - i < 3 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) --i;  // Folgebytes überspringen
  if (i == 0) return;
  const uint8_t lead = (uint8_t)s[i - 1];
  if (lead < 0xC0) return;  // ASCII oder einzelnes Folgebyte: nichts zu tun
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
  if (n - (i - 1) < need) s[i - 1] = 0;
}

// Wie strlcpy, kürzt aber nur an Zeichengrenzen
inline void copy(char* dst, size_t size, const char* src) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = 0;
  if (strlen(src) >= size) trimIncomplete(dst);
}

}  // namespace utf8
