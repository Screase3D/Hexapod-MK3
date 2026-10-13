// Test: Ringpuffer des Verlaufs (sys/history.h)
#include <cstdio>

#include "sys/history.h"

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::printf("FEHLER: %s\n", what);
  }
}

int main() {
  history::Ring<4> r;
  check(r.count() == 0, "leer");
  for (int i = 1; i <= 3; ++i) r.push({(uint16_t)(7000 + i), -50, 40, 100});
  check(r.count() == 3, "3 Werte");
  check(r.at(0).bat_mv == 7001 && r.at(2).bat_mv == 7003, "älteste zuerst");
  for (int i = 4; i <= 10; ++i) r.push({(uint16_t)(7000 + i), -50, 40, 100});
  check(r.count() == 4, "voll: 4 Werte");
  check(r.at(0).bat_mv == 7007 && r.at(3).bat_mv == 7010, "nach Überlauf: die neuesten 4, älteste zuerst");
  history::Ring<history::SAMPLES> big;
  for (int i = 0; i < 500; ++i) big.push({(uint16_t)i, 0, 0, 0});
  check(big.count() == history::SAMPLES && big.at(history::SAMPLES - 1).bat_mv == 499 &&
            big.at(0).bat_mv == 500 - history::SAMPLES, "180 Werte nach 500");
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
