#include "cam_zoom.h"

namespace camzoom {
namespace {
constexpr float MAX_ZOOM = 4.0f;

struct Ratio {
  int mw, mh, sx, sy, ex, ey, ox, oy, tx, ty;
};
// = ratio_table aus esp32-camera (ov5640_settings.h), Reihenfolge wie aspect_ratio_t
constexpr Ratio RATIOS[] = {
    {2560, 1920, 0, 0, 2623, 1951, 32, 16, 2844, 1968},     // 4:3
    {2560, 1704, 0, 110, 2623, 1843, 32, 16, 2844, 1752},   // 3:2
    {2560, 1600, 0, 160, 2623, 1791, 32, 16, 2844, 1648},   // 16:10
    {2560, 1536, 0, 192, 2623, 1759, 32, 16, 2844, 1584},   // 5:3
    {2560, 1440, 0, 240, 2623, 1711, 32, 16, 2844, 1488},   // 16:9
    {2560, 1080, 0, 420, 2623, 1531, 32, 16, 2844, 1128},   // 21:9
    {2400, 1920, 80, 0, 2543, 1951, 32, 16, 2684, 1968},    // 5:4
    {1920, 1920, 320, 0, 2543, 1951, 32, 16, 2684, 1968},   // 1:1
    {1088, 1920, 736, 0, 1887, 1951, 32, 16, 1884, 1968},   // 9:16
};
constexpr int NUM_RATIOS = sizeof(RATIOS) / sizeof(RATIOS[0]);

float minf(float a, float b) { return a < b ? a : b; }
}  // namespace

float maxZoom(int w, int h, int ratio) {
  if (ratio < 0 || ratio >= NUM_RATIOS || w <= 0 || h <= 0) return 1.0f;
  const Ratio& r = RATIOS[ratio];
  const float m = minf(MAX_ZOOM, minf(static_cast<float>(r.mw) / w, static_cast<float>(r.mh) / h));
  return m < 1.0f ? 1.0f : m;
}

bool window(int w, int h, int ratio, float z, Window& out) {
  if (ratio < 0 || ratio >= NUM_RATIOS || w <= 0 || h <= 0) return false;
  const Ratio& r = RATIOS[ratio];
  const float zmax = maxZoom(w, h, ratio);
  if (!(z >= 1.0f)) z = 1.0f;  // auch NaN
  if (z > zmax) z = zmax;
  // Fenster: Breite auf 16 Pixel, Höhe im gleichen Seitenverhältnis (gerade), nie kleiner als die Ausgabe
  int ww = static_cast<int>(r.mw / z) & ~15;
  if (ww < w) ww = w;
  if (ww > r.mw) ww = r.mw;
  int wh = static_cast<int>(static_cast<long>(ww) * r.mh / r.mw) & ~1;
  if (wh < h) wh = h;
  if (wh > r.mh) wh = r.mh;
  out.sx = r.sx + (((r.mw - ww) / 2) & ~1);
  out.sy = r.sy + (((r.mh - wh) / 2) & ~1);
  out.ex = out.sx + ww + (r.ex - r.sx - r.mw);  // Rand wie beim vollen Fenster
  out.ey = out.sy + wh + (r.ey - r.sy - r.mh);
  out.binning = w <= ww / 2 && h <= wh / 2;
  out.scale = !((w == ww && h == wh) || (w == ww / 2 && h == wh / 2));
  if (out.binning) {
    out.tx = w > 920 ? r.tx - 200 : 2060;
    out.ty = r.ty / 2;
    out.ox = r.ox / 2;
    out.oy = r.oy / 2;
  } else {
    out.tx = r.tx;
    // kleineres Fenster: weniger Zeilen pro Bild (sonst sinkt die Bildrate ohne Binning stark),
    // aber nie kürzer als im Binning-Betrieb des Treibers
    out.ty = ww < r.mw ? (wh + (r.ty - r.mh) > r.ty / 2 ? wh + (r.ty - r.mh) : r.ty / 2) : r.ty;
    out.ox = r.ox;
    out.oy = r.oy;
  }
  out.zoom = static_cast<float>(r.mw) / ww;
  return true;
}

}  // namespace camzoom
