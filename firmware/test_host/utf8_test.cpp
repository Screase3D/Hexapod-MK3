// Test: Texte nur an UTF-8-Zeichengrenzen kürzen (sys/utf8.h)
#include <cstdio>
#include <cstring>

#include "sys/utf8.h"

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    printf("FEHLER: %s\n", what);
  }
}

// gültiges UTF-8 (vollständige Zeichen)?
static bool valid(const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p;) {
    int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
    if (!n) return false;
    for (int i = 1; i < n; ++i) {
      if ((p[i] & 0xC0) != 0x80) return false;
    }
    p += n;
  }
  return true;
}

int main() {
  // ASCII bleibt
  char a[] = "Hexapod";
  utf8::trimIncomplete(a);
  check(strcmp(a, "Hexapod") == 0, "ASCII unverändert");

  // vollständige Umlaute bleiben
  char b[] = "Körper";
  utf8::trimIncomplete(b);
  check(strcmp(b, "Körper") == 0, "vollständiges ö bleibt");

  // halbes ö (2 Bytes) am Ende wird entfernt
  char c[] = "K\xC3";
  utf8::trimIncomplete(c);
  check(strcmp(c, "K") == 0, "halbes ö entfernt");

  // halber Gedankenstrich (3 Bytes: E2 80 93), 1 bzw. 2 Bytes vorhanden
  char d[] = "a \xE2";
  utf8::trimIncomplete(d);
  check(strcmp(d, "a ") == 0, "1/3 Gedankenstrich entfernt");
  char e[] = "a \xE2\x80";
  utf8::trimIncomplete(e);
  check(strcmp(e, "a ") == 0, "2/3 Gedankenstrich entfernt");
  char f[] = "a \xE2\x80\x93";
  utf8::trimIncomplete(f);
  check(strcmp(f, "a \xE2\x80\x93") == 0, "ganzer Gedankenstrich bleibt");

  // copy: jede Puffergröße ergibt gültiges UTF-8 und einen Präfix des Originals
  const char* src = "Servos an – zum Aufstehen noch einmal drücken (Größe ß, € 4-Byte: \xF0\x9F\x98\x80)";
  for (size_t size = 1; size <= strlen(src) + 2; ++size) {
    char buf[128];
    utf8::copy(buf, size, src);
    char what[64];
    snprintf(what, sizeof what, "copy mit Puffer %u", (unsigned)size);
    check(valid(buf) && strncmp(buf, src, strlen(buf)) == 0 && strlen(buf) < size, what);
    check(strlen(buf) + 4 > (size - 1 < strlen(src) ? size - 1 : strlen(src)), what);  // höchstens 3 Bytes verloren
  }

  printf("%d Prüfungen, %d Fehler\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
