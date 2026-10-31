# Hardware and Pin Assignment

Source: [`hexapod_schaltplan_v4.png`](hexapod_schaltplan_v4.png) and
[`ESP32_S3_Camera_Instructions.pdf`](ESP32_S3_Camera_Instructions.pdf) (page “Product Pin Description”).
All pins are defined centrally in [`firmware/include/config.h`](../firmware/include/config.h).

## Components

| Assembly | Type | Note |
|---|---|---|
| Controller | ESP32-S3-CAM (ESP32-S3 N16R8) | 16 MB flash, 8 MB octal PSRAM, CH340 on the “TTL” port |
| Camera | OV5640 (fitted), OV2640 according to the board docs | Type is detected automatically; the OV5640 gets very hot in continuous operation → the firmware puts it to sleep when not streaming; a heat sink/airflow on the module helps |
| Servo driver | 2× PCA9685 | #1 = 0x40 (left side), #2 = 0x41 (A0 bridged, right side) |
| Servos | 18× MG90S | Coxa / femur / tibia per leg |
| IMU | MPU6050 (GY-521) | 0x68 (AD0 to GND), on 3V3 |
| Power supply | 2× 18650 in series (2S, **no BMS**), 7.5 A fuse | V_BAT 6.0–8.4 V |
| Servo supply | 2× UBEC at 5 V | one UBEC per PCA9685 (9 servos) |
| Logic supply | Mini buck 5 V | ESP32 + LEDs |
| Eyes | 2× WS2812B | via diode D1 (≈ 4.3 V) and 330 Ω |

## ESP32 pins

| GPIO | Function | Remark |
|---|---|---|
| 47 | I²C SDA | PCA9685 #1/#2, MPU6050 (Wire, port 0, 400 kHz) |
| 21 | I²C SCL | |
| 1 | Battery voltage (ADC1_CH0) | Divider R1 100 kΩ / R2 47 kΩ, 100 nF → factor 3.128 |
| 14 | WS2812B eyes (data) | RMT |
| 48 | WS2812 on the board | Status indicator by default only at startup and while connecting to Wi-Fi, then off (sits in front of the camera); *LEDs* page: also “like the eyes” or completely off |
| 0 | BOOT button | currently unused |
| 4 / 5 | Camera SCCB SDA / SCL | separate I²C port 1 |
| 15 | Camera XCLK | 20 MHz |
| 16, 17, 18, 12, 10, 8, 9, 11 | Camera Y9 … Y2 (D7 … D0) | |
| 6 / 7 / 13 | Camera VSYNC / HREF / PCLK | |
| 38, 39, 40 | SD card (CMD, CLK, DATA) | free for future recording |
| 35, 36, 37 | Octal PSRAM | **do not use** |
| 43 / 44 | UART0 TX / RX (CH340) | Serial monitor, 115200 baud |

## Servo mapping

Servo ID = leg × 3 + joint (joint 0 = coxa, 1 = femur, 2 = tibia).

| Leg (UI) | Position | PCA9685 | Channels coxa / femur / tibia | Servo IDs |
|---|---|---|---|---|
| 1 | front left | #2 (0x41) | CH0 / CH1 / CH2 | 0 / 1 / 2 |
| 2 | middle left | #2 (0x41) | CH3 / CH4 / CH5 | 3 / 4 / 5 |
| 3 | rear left | #2 (0x41) | CH6 / CH7 / CH8 | 6 / 7 / 8 |
| 4 | front right | #1 (0x40) | CH0 / CH1 / CH2 | 9 / 10 / 11 |
| 5 | middle right | #1 (0x40) | CH3 / CH4 / CH5 | 12 / 13 / 14 |
| 6 | rear right | #1 (0x40) | CH6 / CH7 / CH8 | 15 / 16 / 17 |

The two PCA9685 boards are mounted mirrored: **#1 (0x40) sits on the right** and, via
UBEC 1, supplies legs 4–6; **#2 (0x41, A0 bridged) sits on the left** with UBEC 2 and legs 1–3.

If the wiring differs, only adjust the `SERVO_MAP` table in
[`firmware/src/servo/servo_map.h`](../firmware/src/servo/servo_map.h).

## Circuit notes

- **Battery without BMS:** The firmware can protect the cells in three stages:
  warning below 6.8 V, servos off below 6.4 V (after 5 s), ESP32 deep sleep below 6.0 V
  (after 10 s). The UBECs and buck still draw quiescent current even then – so disconnect an empty battery
  promptly anyway. Thresholds in `config.h` (`BAT_*`).
- **Switching battery monitoring:** Under *Settings* → *Battery* there are two switches, stored in NVS:
  *Battery monitoring* (warning + servo shutdown) and *Deep sleep on deep discharge*
  (only effective with monitoring switched on). **Monitoring is off by factory default**, because
  GPIO 1 is floating without a fitted voltage divider and returns random values. The voltage is still measured and
  displayed (“Reading (unchecked)”): after soldering in the divider, compare the value
  with a multimeter at the battery and only then switch it on. The factory default
  is set in `BAT_MONITOR_DEFAULT` / `BAT_DEEP_SLEEP_DEFAULT`.
- **No OE pin wired:** The PCA9685 outputs are switched off via I²C
  (“Torque off”, emergency stop). After power-up all outputs are off until torque is enabled in the
  UI.
- **Staggered power-up:** When torque is enabled, another servo is switched on every 20 ms,
  and the pulse start times of the channels are distributed across the PWM period. This
  limits the current peaks on the UBECs.
- **Safe power-up:** The actual position of the servos is unknown before power-up. Each
  servo therefore first goes to the sitting pose (does not push up a robot lying down) and moves
  from there at no more than 45 °/s to its target – regardless of which target last applied (e.g.
  the assembly pose). The calibration's “All …” commands move just as slowly.
- **PCA9685 oscillator:** nominally 25 MHz, in practice often 24–27 MHz. Calibration
  compensates for this; if you want it exact, measure 1500 µs with an oscilloscope/logic analyzer and enter the
  corrected value in `PCA1_OSC_HZ` / `PCA2_OSC_HZ`.
- **ESP supply:** either mini buck or USB, not both without a Schottky diode
  (see schematic). So to flash via USB, switch off the main switch SW1 – or use a
  USB cable or adapter without a 5 V line if the battery is to stay connected.
