// Tests für den Sensor-Zoom des OV5640 (Fensterberechnung) auf dem PC:
//   g++ -std=c++17 -I../src camzoom_test.cpp ../src/net/cam_zoom.cpp -o camzoom_test && ./camzoom_test
// Läuft auch in der CI (.github/workflows/firmware.yml).

#include <cmath>
#include <cstdio>

#include "net/cam_zoom.h"

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failed;                                                   \
      std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

int main() {
  using camzoom::Window;
  const int R4X3 = 0, R16X9 = 4;
  Window w;
  // 1×: genau wie der Treiber bei VGA (4:3, Binning) und UXGA (ohne Binning)
  CHECK(camzoom::window(640, 480, R4X3, 1.0f, w));
  CHECK(w.sx == 0 && w.sy == 0 && w.ex == 2623 && w.ey == 1951);
  CHECK(w.binning && w.scale && w.tx == 2060 && w.ty == 984 && w.ox == 16 && w.oy == 8 && w.zoom == 1.0f);
  CHECK(camzoom::window(1600, 1200, R4X3, 1.0f, w));
  CHECK(!w.binning && w.scale && w.tx == 2844 && w.ty == 1968 && w.ox == 32 && w.oy == 16);
  CHECK(camzoom::window(1280, 720, R16X9, 1.0f, w));
  CHECK(w.sy == 240 && w.ey == 1711 && w.binning && w.tx == 2844 - 200 && w.ty == 1488 / 2);

  // Zoom-Grenzen: VGA 4×, SVGA 3,2×, UXGA 1,6×, HD 2×
  CHECK(camzoom::maxZoom(640, 480, R4X3) == 4.0f);
  CHECK(std::fabs(camzoom::maxZoom(800, 600, R4X3) - 3.2f) < 1e-4f);
  CHECK(std::fabs(camzoom::maxZoom(1600, 1200, R4X3) - 1.6f) < 1e-4f);
  CHECK(camzoom::maxZoom(1280, 720, R16X9) == 2.0f);
  CHECK(camzoom::maxZoom(320, 240, R4X3) == 4.0f);

  // 2× bei VGA: mittiger Ausschnitt 1280×960, Binning, Rand und Seitenverhältnis wie beim vollen Fenster
  CHECK(camzoom::window(640, 480, R4X3, 2.0f, w));
  CHECK(w.ex - w.sx - 63 == 1280 && w.ey - w.sy - 31 == 960);
  CHECK(w.sx == 640 && w.sy == 480);
  CHECK(w.binning && !w.scale && w.zoom == 2.0f);
  // 4× bei VGA: Ausschnitt 640×480 = Ausgabe, weder Binning noch Skalierung
  CHECK(camzoom::window(640, 480, R4X3, 4.0f, w));
  CHECK(w.ex - w.sx - 63 == 640 && w.ey - w.sy - 31 == 480 && !w.binning && !w.scale);
  CHECK(w.tx == 2844 && w.ty == 984 && w.ox == 32);  // Bildhöhe wie im Binning-Betrieb, nicht 1968
  // zu viel: auf das Maximum begrenzt; zu wenig / NaN: 1×
  CHECK(camzoom::window(1600, 1200, R4X3, 4.0f, w) && std::fabs(w.zoom - 1.6f) < 1e-3f);
  CHECK(w.ex - w.sx - 63 >= 1600 && w.ey - w.sy - 31 >= 1200);
  CHECK(w.ty == w.ey - w.sy - 31 + 48 && !w.binning);
  CHECK(camzoom::window(640, 480, R4X3, 0.3f, w) && w.zoom == 1.0f);
  CHECK(camzoom::window(640, 480, R4X3, NAN, w) && w.zoom == 1.0f);
  // Zwischenwerte: Fenster bleibt mittig, gerade Startwerte, Breite auf 16 Pixel, nie kleiner als die Ausgabe
  for (float z = 1.0f; z <= 4.0f; z += 0.05f) {
    for (int fs = 0; fs < 3; ++fs) {
      const int ow[] = {320, 640, 800}, oh[] = {240, 480, 600};
      CHECK(camzoom::window(ow[fs], oh[fs], R4X3, z, w));
      const int ww = w.ex - w.sx - 63, wh = w.ey - w.sy - 31;
      CHECK(ww >= ow[fs] && wh >= oh[fs] && ww % 16 == 0 && w.sx % 2 == 0 && w.sy % 2 == 0);
      CHECK(std::abs((2560 - ww) - 2 * w.sx) <= 2 && std::abs((1920 - wh) - 2 * w.sy) <= 2);  // mittig
      CHECK(std::fabs(static_cast<float>(ww) / wh - 4.0f / 3.0f) < 0.01f);                      // nicht verzerrt
      CHECK(w.zoom >= 1.0f && w.zoom <= camzoom::maxZoom(ow[fs], oh[fs], R4X3) + 1e-3f);
    }
  }
  // Rand abschneiden: VGA, 20 % unten (Y hoch) -> Fenster 2048×1536 oben bündig, mittig in x
  using camzoom::Edge;
  CHECK(camzoom::window(640, 480, R4X3, 1.0f, w, 0.2f, Edge::YHigh));
  CHECK(w.ex - w.sx - 63 == 2048 && w.ey - w.sy - 31 == 1536 && w.sy == 0 && w.sx == 256);
  CHECK(w.binning && std::fabs(w.crop - 0.2f) < 1e-6f && w.zoom == 1.0f);
  CHECK(camzoom::window(640, 480, R4X3, 1.0f, w, 0.2f, Edge::YLow));
  CHECK(w.sy == 384 && w.ey == 1920 + 31);  // unten bündig
  CHECK(camzoom::window(640, 480, R4X3, 1.0f, w, 0.2f, Edge::XHigh));
  CHECK(w.sx == 0 && w.sy == 192);
  CHECK(camzoom::window(640, 480, R4X3, 1.0f, w, 0.2f, Edge::XLow));
  CHECK(w.sx == 512 && w.ex == 2560 + 63);
  // dazu 2× Zoom: mittig im verbleibenden Bild, nie im abgeschnittenen Streifen
  CHECK(camzoom::window(640, 480, R4X3, 2.0f, w, 0.2f, Edge::YHigh));
  CHECK(w.ex - w.sx - 63 == 1024 && w.sy == 384 && w.sy + 768 <= 1536 && w.zoom == 2.0f);
  // Grenzen: weniger Zoom übrig; UXGA höchstens 37,5 % (Fenster nie kleiner als 1600×1200)
  CHECK(std::fabs(camzoom::maxZoom(640, 480, R4X3, 0.2f) - 3.2f) < 1e-4f);
  CHECK(std::fabs(camzoom::maxCrop(1600, 1200, R4X3) - 0.375f) < 1e-4f);
  CHECK(camzoom::window(1600, 1200, R4X3, 1.0f, w, 0.5f, Edge::YHigh));
  CHECK(std::fabs(w.crop - 0.375f) < 1e-4f && w.ex - w.sx - 63 == 1600 && w.ey - w.sy - 31 == 1200 && w.sy == 0);
  CHECK(!w.scale && !w.binning);
  for (float c = 0.0f; c <= 0.4f; c += 0.01f) {
    for (int e = 0; e < 4; ++e) {
      for (float z = 1.0f; z <= 4.0f; z += 0.25f) {
        CHECK(camzoom::window(800, 600, R4X3, z, w, c, static_cast<Edge>(e)));
        const int ww = w.ex - w.sx - 63, wh = w.ey - w.sy - 31;
        CHECK(ww >= 800 && wh >= 600 && w.sx >= 0 && w.sy >= 0 && w.sx + ww <= 2560 && w.sy + wh <= 1920);
        CHECK(w.sx % 2 == 0 && w.sy % 2 == 0);
        // nichts aus dem abgeschnittenen Streifen (2 Pixel Rundung)
        const float cut = w.crop * (e < 2 ? 1920 : 2560);
        if (e == 0) CHECK(w.sy + wh <= 1920 - cut + 2);
        if (e == 1) CHECK(w.sy >= cut - 2);
        if (e == 2) CHECK(w.sx + ww <= 2560 - cut + 2);
        if (e == 3) CHECK(w.sx >= cut - 2);
      }
    }
  }
  // unbekanntes Seitenverhältnis
  CHECK(!camzoom::window(640, 480, 42, 2.0f, w));
  std::printf("%d Prüfungen, %d Fehler\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
