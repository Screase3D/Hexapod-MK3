#pragma once
// Kamera (OV2640/OV5640) + MJPEG-Stream auf eigenem HTTP-Server (Port 81),
// damit ein laufender Stream die Steuerung auf Port 80 nicht blockiert.
//
//   http://<ip>:81/stream   MJPEG (multipart/x-mixed-replace)
//   http://<ip>:81/capture  Einzelbild (JPEG)

#include <Arduino.h>
#include <ArduinoJson.h>

namespace camera {

struct Settings {
  int framesize;  // framesize_t (z.B. FRAMESIZE_VGA)
  int quality;    // JPEG-Qualität 4..63 (kleiner = besser)
  bool vflip;
  bool hmirror;
  int max_fps;    // Bildratenbegrenzung des Streams
};

bool begin();
bool ok();
bool startStreamServer();

Settings settings();
// Übernimmt nur die im JSON vorhandenen Felder; persistiert in NVS.
bool applyJson(JsonVariantConst obj, String& err);
void toJson(JsonObject obj);

}  // namespace camera
