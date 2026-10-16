#pragma once
// Zoom am OV5640: statt des ganzen Sensorbilds (2560×1920) einen mittigen Ausschnitt auslesen
// und auf die Stream-Auflösung bringen – echter Zoom mit mehr Details (bei VGA bis 4×), nicht
// nur vergrößerte Pixel. Dazu Rand abschneiden: Ein Streifen an einer Seite des Sensors (z. B. das,
// was die Kamera vom Roboter selbst sieht) wird gar nicht ausgelesen; das Bild behält sein
// Seitenverhältnis, an den beiden Nachbarseiten fällt dafür je die Hälfte so viel weg. Reine Rechnung ohne Arduino (Tests: test_host/camzoom_test.cpp);
// Werte wie im Treiber esp32-camera (ov5640.c set_framesize, ov5640_settings.h ratio_table).

namespace camzoom {

// Register-Werte für sensor_t::set_res_raw
struct Window {
  int sx, sy, ex, ey;  // Sensorfenster (X/Y_ADDR_ST/END)
  int ox, oy;          // ISP-Versatz im Fenster
  int tx, ty;          // Zeilenlänge/Bildhöhe inkl. Austastung (HTS/VTS)
  bool scale;          // ISP skaliert
  bool binning;        // 2×2 zusammenfassen
  float zoom;          // tatsächlich eingestellter Zoom (bezogen auf das Bild nach dem Abschneiden)
  float crop;          // tatsächlich abgeschnittener Anteil
};

// Abzuschneidende Seite im Sensor-Array (Adressen): Y/X hoch bzw. niedrig
enum class Edge : unsigned char { YHigh, YLow, XHigh, XLow };

// Größter abschneidbarer Anteil für die Ausgabe w×h (das Fenster darf nicht kleiner als sie werden)
float maxCrop(int w, int h, int ratio);
// Größter Zoom für die Ausgabe w×h mit Seitenverhältnis ratio (aspect_ratio_t), höchstens 4×,
// nach dem Abschneiden des Anteils crop
float maxZoom(int w, int h, int ratio, float crop = 0.0f);

// Fenster für Zoom z (wird auf 1..maxZoom begrenzt) im Bild ohne den abgeschnittenen Streifen
// (Anteil crop an der Seite edge, auf maxCrop begrenzt). false: Seitenverhältnis unbekannt.
// Bei z = 1 ohne Abschneiden genau die Werte, die der Treiber für die Auflösung setzt.
bool window(int w, int h, int ratio, float z, Window& out, float crop = 0.0f, Edge edge = Edge::YHigh);

}  // namespace camzoom
