#pragma once
// Kamera (OV2640/OV5640) + MJPEG-Stream auf eigenem HTTP-Server (Port 81),
// damit ein laufender Stream die Steuerung auf Port 80 nicht blockiert.
//
//   http://<ip>:81/stream   MJPEG (multipart/x-mixed-replace)
//   http://<ip>:81/capture  Einzelbild (JPEG)
//
// Ruhemodus: Ohne Stream bzw. /capture schläft die Kamera (Sensor im
// Software-Power-Down, Takt und Bildaufnahme aus) – sonst wird z. B. der
// OV5640 sehr heiß. Der erste Abruf weckt sie wieder (ca. 0,5-1 s).
//
// Drehung: 180° erledigt der Sensor selbst (Spiegeln in beide Richtungen).
// 90°/270° kann der Sensor nicht; dafür liefert er 0°/180° und die
// Oberfläche dreht das Bild zusätzlich um 90° (CSS). Stream und /capture
// enthalten bei 90°/270° also das um 90° "vorgedrehte" Bild.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace camera {

struct Settings {
  int framesize;  // framesize_t (z.B. FRAMESIZE_VGA)
  int quality;    // JPEG-Qualität 4..63 (kleiner = besser)
  bool vflip;
  bool hmirror;
  int max_fps;    // Bildratenbegrenzung des Streams
  int rotation;   // 0/90/180/270 Grad im Uhrzeigersinn (Einbaulage der Kamera)
  int xclk_mhz;   // Kamera-Takt 10/16/20 MHz (wirkt beim nächsten Aufwecken)
  // Höchstens diese Auflösung, solange Bluetooth an ist (0 = keine Anpassung): WLAN und
  // Bluetooth teilen sich die Antenne, große Bilder kämen dann nur mit 4–5 Bildern/s an
  int bt_framesize;
  // Bild (Sensor): -2..2, 0 = Werkseinstellung
  int brightness, contrast, saturation, sharpness;  // Schärfe nur OV5640/OV3660
  int ae_level;     // Belichtungskorrektur
  int gainceiling;  // größte Verstärkung 0..6 = 2x..128x (mehr = heller, aber mehr Rauschen)
  int wb_mode;      // Weißabgleich: 0 auto, 1 Sonne, 2 Wolken, 3 Büro, 4 Zuhause
  bool aec2;        // Nachtmodus: Belichtung über den Bildprozessor (länger, heller)
  bool lenc;        // Linsenkorrektur (dunkle Ränder ausgleichen)
  bool bpc;         // Pixelfehler-Korrektur (helle/dunkle Punkte)
  // Rand abschneiden (Kamera sieht z. B. die Platine vor sich): Anteil 0..50 % und Seite im
  // angezeigten Bild (0 unten, 1 oben). Das Bild wird um den Streifen kleiner (nichts wird
  // vergrößert). OV5640: im Sensor, sonst blendet ihn die Oberfläche aus.
  int crop;
  int crop_edge;
};

bool begin();
bool ok();  // Kamera beim Start erkannt (auch wenn sie gerade schläft)
bool startStreamServer();
// Legt die Kamera schlafen, wenn sie CAM_IDLE_SLEEP_MS nicht gebraucht wurde.
// Aus loop() aufrufen.
void loop();

Settings settings();
// Übernimmt nur die im JSON vorhandenen Felder; persistiert in NVS.
bool applyJson(JsonVariantConst obj, String& err);
void toJson(JsonObject obj);
// Live-Statistik des Streams (Bildrate, Bildgröße, Datenrate, Fehler)
void statsToJson(JsonObject obj);

}  // namespace camera
