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

// Start (gerade) eines Fensters der Größe size mittig im Bereich [lo, lo+len] des Sensors (0..full)
int place(float lo, float len, int size, int full) {
  int s = static_cast<int>(lo + 0.5f * (len - size)) & ~1;
  const int hi = static_cast<int>(lo + len) - size;  // nicht in den abgeschnittenen Streifen
  if (s > hi) s = hi & ~1;
  if (s < static_cast<int>(lo)) s = (static_cast<int>(lo) + 1) & ~1;
  if (s > full - size) s = (full - size) & ~1;
  return s < 0 ? 0 : s;
}
}  // namespace

float maxZoom(int w, int h, int ratio) {
  if (ratio < 0 || ratio >= NUM_RATIOS || w <= 0 || h <= 0) return 1.0f;
  const Ratio& r = RATIOS[ratio];
  const float m = minf(MAX_ZOOM, minf(static_cast<float>(r.mw) / w, static_cast<float>(r.mh) / h));
  return m < 1.0f ? 1.0f : m;
}

bool window(int w, int h, int ratio, float z, Window& out, float crop, Edge edge) {
  if (ratio < 0 || ratio >= NUM_RATIOS || w <= 0 || h <= 0) return false;
  const Ratio& r = RATIOS[ratio];
  if (!(crop > 0.0f)) crop = 0.0f;  // auch NaN
  crop = minf(crop, MAX_CROP);
  const bool cut_y = edge == Edge::YHigh || edge == Edge::YLow;
  // Bildgröße: um den Streifen kleiner (auf 16 Pixel), sonst wie die Auflösung
  int ow = w, oh = h;
  if (crop > 0.0f) {
    if (cut_y) oh = static_cast<int>(h * (1.0f - crop)) & ~15;
    else ow = static_cast<int>(w * (1.0f - crop)) & ~15;
    if (oh < 16) oh = 16;
    if (ow < 16) ow = 16;
  }
  const float zmax = maxZoom(w, h, ratio);
  if (!(z >= 1.0f)) z = 1.0f;  // auch NaN
  if (z > zmax) z = zmax;
  // Bereich ohne den abgeschnittenen Streifen (gleicher Maßstab wie das ganze Bild, an der Gegenseite bündig)
  const float rw = static_cast<float>(r.mw) * ow / w, rh = static_cast<float>(r.mh) * oh / h;
  float rx = 0.5f * (r.mw - rw), ry = 0.5f * (r.mh - rh);
  switch (edge) {
    case Edge::YHigh: ry = 0.0f; break;
    case Edge::YLow:  ry = r.mh - rh; break;
    case Edge::XHigh: rx = 0.0f; break;
    case Edge::XLow:  rx = r.mw - rw; break;
  }
  // Fenster: Breite auf 16 Pixel, Höhe im Seitenverhältnis des Bilds (gerade), nie kleiner als das Bild
  int ww = static_cast<int>(rw / z) & ~15;
  if (ww < ow) ww = ow;
  if (ww > r.mw) ww = r.mw;
  int wh = 2 * static_cast<int>(0.5f * ww * oh / ow + 0.5f);  // gerade, gerundet (nicht verzerren)
  if (wh < oh) wh = oh;
  if (wh > r.mh) wh = r.mh;
  out.sx = r.sx + place(rx, rw, ww, r.mw);
  out.sy = r.sy + place(ry, rh, wh, r.mh);
  out.ex = out.sx + ww + (r.ex - r.sx - r.mw);  // Rand wie beim vollen Fenster
  out.ey = out.sy + wh + (r.ey - r.sy - r.mh);
  out.binning = ow <= ww / 2 && oh <= wh / 2;
  out.scale = !((ow == ww && oh == wh) || (ow == ww / 2 && oh == wh / 2));
  if (out.binning) {
    out.tx = ow > 920 ? r.tx - 200 : 2060;
    out.ty = r.ty / 2;
    out.ox = r.ox / 2;
    out.oy = r.oy / 2;
  } else {
    out.tx = r.tx;
    // kleineres Fenster: weniger Zeilen pro Bild (sonst sinkt die Bildrate ohne Binning stark),
    // aber nie kürzer als im Binning-Betrieb des Treibers
    out.ty = wh < r.mh ? (wh + (r.ty - r.mh) > r.ty / 2 ? wh + (r.ty - r.mh) : r.ty / 2) : r.ty;
    out.ox = r.ox;
    out.oy = r.oy;
  }
  out.ow = ow;
  out.oh = oh;
  out.zoom = rw / ww;
  out.crop = cut_y ? 1.0f - static_cast<float>(oh) / h : 1.0f - static_cast<float>(ow) / w;
  return true;
}

}  // namespace camzoom
