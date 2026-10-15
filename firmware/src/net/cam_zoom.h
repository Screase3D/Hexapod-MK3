#pragma once
// Zoom am OV5640: statt des ganzen Sensorbilds (2560×1920) einen mittigen Ausschnitt auslesen
// und auf die Stream-Auflösung bringen – echter Zoom mit mehr Details (bei VGA bis 4×), nicht
// nur vergrößerte Pixel. Reine Rechnung ohne Arduino (Tests: test_host/camzoom_test.cpp);
// Werte wie im Treiber esp32-camera (ov5640.c set_framesize, ov5640_settings.h ratio_table).

namespace camzoom {

// Register-Werte für sensor_t::set_res_raw
struct Window {
  int sx, sy, ex, ey;  // Sensorfenster (X/Y_ADDR_ST/END)
  int ox, oy;          // ISP-Versatz im Fenster
  int tx, ty;          // Zeilenlänge/Bildhöhe inkl. Austastung (HTS/VTS)
  bool scale;          // ISP skaliert
  bool binning;        // 2×2 zusammenfassen
  float zoom;          // tatsächlich eingestellter Zoom
};

// Größter Zoom für die Ausgabe w×h mit Seitenverhältnis ratio (aspect_ratio_t), höchstens 4×
float maxZoom(int w, int h, int ratio);

// Fenster für Zoom z (wird auf 1..maxZoom begrenzt). false: Seitenverhältnis unbekannt.
// Bei z = 1 genau die Werte, die der Treiber für die Auflösung setzt.
bool window(int w, int h, int ratio, float z, Window& out);

}  // namespace camzoom
