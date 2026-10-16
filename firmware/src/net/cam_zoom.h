#pragma once
// Zoom am OV5640: statt des ganzen Sensorbilds (2560×1920) einen mittigen Ausschnitt auslesen
// und auf die Stream-Auflösung bringen – echter Zoom mit mehr Details (bei VGA bis 4×), nicht
// nur vergrößerte Pixel. Dazu Rand abschneiden: Ein Streifen an einer Seite des Sensors (z. B. das,
// was die Kamera vom Roboter selbst sieht) wird gar nicht ausgelesen; das Bild wird um ihn
// kleiner (z. B. 640×320 statt 640×480), der Rest bleibt im gleichen Maßstab. Reine Rechnung ohne Arduino (Tests: test_host/camzoom_test.cpp);
// Werte wie im Treiber esp32-camera (ov5640.c set_framesize, ov5640_settings.h ratio_table).

namespace camzoom {

// Register-Werte für sensor_t::set_res_raw
struct Window {
  int sx, sy, ex, ey;  // Sensorfenster (X/Y_ADDR_ST/END)
  int ox, oy;          // ISP-Versatz im Fenster
  int tx, ty;          // Zeilenlänge/Bildhöhe inkl. Austastung (HTS/VTS)
  bool scale;          // ISP skaliert
  bool binning;        // 2×2 zusammenfassen
  int ow, oh;          // Bildgröße (ohne Abschneiden = Auflösung)
  float zoom;          // tatsächlich eingestellter Zoom
  float crop;          // tatsächlich abgeschnittener Anteil
};

// Abzuschneidende Seite im Sensor-Array (Adressen): Y/X hoch bzw. niedrig
enum class Edge : unsigned char { YHigh, YLow, XHigh, XLow };

constexpr float MAX_CROP = 0.5f;

// Größter Zoom für die Ausgabe w×h mit Seitenverhältnis ratio (aspect_ratio_t), höchstens 4×
// (Abschneiden ändert ihn nicht: Bild und Fenster werden gleich viel kleiner)
float maxZoom(int w, int h, int ratio);

// Fenster für Zoom z (wird auf 1..maxZoom begrenzt) im Bild ohne den abgeschnittenen Streifen
// (Anteil crop an der Seite edge, höchstens MAX_CROP; die Bildgröße wird auf 16 Pixel gerundet).
// false: Seitenverhältnis unbekannt. Bei z = 1 ohne Abschneiden genau die Werte, die der Treiber
// für die Auflösung setzt.
bool window(int w, int h, int ratio, float z, Window& out, float crop = 0.0f, Edge edge = Edge::YHigh);

}  // namespace camzoom
