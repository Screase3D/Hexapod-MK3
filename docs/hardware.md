# Hardware und Pinbelegung

Quelle: [`hexapod_schaltplan_v3.png`](hexapod_schaltplan_v3.png) und
[`ESP32_S3_Camera_Instructions.pdf`](ESP32_S3_Camera_Instructions.pdf) (Seite „Product Pin Description“).
Alle Pins stehen zentral in [`firmware/include/config.h`](../firmware/include/config.h).

## Komponenten

| Baugruppe | Typ | Anmerkung |
|---|---|---|
| Controller | ESP32-S3-CAM (ESP32-S3 N16R8) | 16 MB Flash, 8 MB Octal-PSRAM, CH340 an der Buchse „TTL“ |
| Kamera | OV2640 laut Board-Doku, OV5640 laut Schaltplan | Typ wird von der Firmware automatisch erkannt |
| Servotreiber | 2× PCA9685 | #1 = 0x40 (linke Seite), #2 = 0x41 (A0 gebrückt, rechte Seite) |
| Servos | 18× MG90S | Coxa / Femur / Tibia je Bein |
| Lagesensor | MPU6050 (GY-521) | 0x68 (AD0 an GND), an 3V3 |
| Stromversorgung | 2× 18650 in Serie (2S, **ohne BMS**), Sicherung 7,5 A | V_BAT 6,0–8,4 V |
| Servo-Versorgung | 2× UBEC auf 5 V | je ein UBEC pro PCA9685 (9 Servos) |
| Logik-Versorgung | Mini-Buck 5 V | ESP32 + LEDs |
| Augen | 2× WS2812B | über Diode D1 (≈ 4,3 V) und 330 Ω |

## ESP32-Pins

| GPIO | Funktion | Bemerkung |
|---|---|---|
| 47 | I²C SDA | PCA9685 #1/#2, MPU6050 (Wire, Port 0, 400 kHz) |
| 21 | I²C SCL | |
| 1 | Akkuspannung (ADC1_CH0) | Teiler R1 100 kΩ / R2 47 kΩ, 100 nF → Faktor 3,128 |
| 14 | WS2812B-Augen (Daten) | RMT |
| 48 | WS2812 auf dem Board | Statusanzeige parallel zu den Augen |
| 0 | BOOT-Taster | derzeit ungenutzt |
| 4 / 5 | Kamera SCCB SDA / SCL | eigener I²C-Port 1 |
| 15 | Kamera XCLK | 20 MHz |
| 16, 17, 18, 12, 10, 8, 9, 11 | Kamera Y9 … Y2 (D7 … D0) | |
| 6 / 7 / 13 | Kamera VSYNC / HREF / PCLK | |
| 38, 39, 40 | SD-Karte (CMD, CLK, DATA) | frei für spätere Aufzeichnung |
| 35, 36, 37 | Octal-PSRAM | **nicht verwenden** |
| 43 / 44 | UART0 TX / RX (CH340) | Serieller Monitor, 115200 Baud |

## Servo-Zuordnung

Servo-ID = Bein × 3 + Gelenk (Gelenk 0 = Coxa, 1 = Femur, 2 = Tibia).

| Bein (UI) | Position | PCA9685 | Kanäle Coxa / Femur / Tibia | Servo-IDs |
|---|---|---|---|---|
| 1 | vorne links | #1 (0x40) | CH0 / CH1 / CH2 | 0 / 1 / 2 |
| 2 | Mitte links | #1 (0x40) | CH3 / CH4 / CH5 | 3 / 4 / 5 |
| 3 | hinten links | #1 (0x40) | CH6 / CH7 / CH8 | 6 / 7 / 8 |
| 4 | vorne rechts | #2 (0x41) | CH0 / CH1 / CH2 | 9 / 10 / 11 |
| 5 | Mitte rechts | #2 (0x41) | CH3 / CH4 / CH5 | 12 / 13 / 14 |
| 6 | hinten rechts | #2 (0x41) | CH6 / CH7 / CH8 | 15 / 16 / 17 |

Weicht die Verkabelung ab, nur die Tabelle `SERVO_MAP` in
[`firmware/src/servo/servo_map.h`](../firmware/src/servo/servo_map.h) anpassen.

## Hinweise zur Schaltung

- **Akku ohne BMS:** Die Firmware kann die Zellen in drei Stufen schützen:
  Warnung unter 6,8 V, Servos aus unter 6,4 V (nach 5 s), Tiefschlaf des ESP32 unter 6,0 V
  (nach 10 s). UBECs und Buck ziehen auch dann noch Ruhestrom – einen leeren Akku also
  trotzdem zeitnah abstecken. Schwellen in `config.h` (`BAT_*`).
- **Akkuüberwachung schalten:** Im Tab *System* gibt es zwei Schalter, gespeichert im NVS:
  *Akkuüberwachung* (Warnung + Servo-Abschaltung) und *Tiefschlaf bei Tiefentladung*
  (wirkt nur bei eingeschalteter Überwachung). **Ab Werk ist die Überwachung aus**, weil
  GPIO 1 ohne bestückten Spannungsteiler offen ist und Zufallswerte liefert. Gemessen und
  angezeigt wird trotzdem („Messwert (ungeprüft)“): Nach dem Einlöten des Teilers den Wert
  mit einem Multimeter am Akku vergleichen und erst dann einschalten. Die Werkseinstellung
  steht in `BAT_MONITOR_DEFAULT` / `BAT_DEEP_SLEEP_DEFAULT`.
- **Kein OE-Pin verdrahtet:** Die PCA9685-Ausgänge werden per I²C abgeschaltet
  („Torque aus“, Not-Aus). Nach dem Einschalten sind alle Ausgänge aus, bis Torque in der
  Oberfläche aktiviert wird.
- **Gestaffeltes Einschalten:** Beim Aktivieren von Torque wird alle 20 ms ein weiterer Servo
  zugeschaltet, und die Pulsanfänge der Kanäle sind über die PWM-Periode verteilt. Das
  begrenzt die Stromspitzen auf den UBECs.
- **PCA9685-Oszillator:** nominal 25 MHz, real oft 24–27 MHz. Über die Kalibrierung wird das
  ausgeglichen; wer es genau will, misst 1500 µs mit Oszilloskop/Logic-Analyzer und trägt den
  korrigierten Wert in `PCA1_OSC_HZ` / `PCA2_OSC_HZ` ein.
- **ESP-Versorgung:** entweder Mini-Buck oder USB, nicht beides ohne Schottky-Diode
  (siehe Schaltplan). Zum Flashen per USB also den Hauptschalter SW1 ausschalten – oder ein
  USB-Kabel bzw. -Adapter ohne 5-V-Leitung verwenden, wenn der Akku dranbleiben soll.
