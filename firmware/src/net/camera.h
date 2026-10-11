#pragma once
// Kamera (OV2640/OV5640) + MJPEG-Stream auf eigenem HTTP-Server (Port 81),
// damit ein laufender Stream die Steuerung auf Port 80 nicht blockiert.
//
//   http://<ip>:81/stream   MJPEG (multipart/x-mixed-replace)
//   http://<ip>:81/capture  Einzelbild (JPEG)
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
  int xclk_mhz;   // Kamera-Takt 10/16/20 MHz (wirkt nach Neustart)
};

bool begin();
bool ok();
bool startStreamServer();

Settings settings();
// Übernimmt nur die im JSON vorhandenen Felder; persistiert in NVS.
bool applyJson(JsonVariantConst obj, String& err);
void toJson(JsonObject obj);
// Live-Statistik des Streams (Bildrate, Bildgröße, Datenrate, Fehler)
void statsToJson(JsonObject obj);

}  // namespace camera
