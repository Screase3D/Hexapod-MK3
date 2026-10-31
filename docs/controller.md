# Xbox controller

Besides touch/keyboard, the hexapod can also be controlled with an Xbox controller – in two ways
(**Controller** page in the UI):

| Way | How | Advantage |
|---|---|---|
| **Bluetooth on the robot** | the controller pairs directly with the ESP32-S3 | no phone needed |
| **Controller on this device** | controller on the phone/PC (Bluetooth or USB), the open page forwards it | works with any controller the browser recognises |

Both use the same mapping, which the robot evaluates (`src/input/pad_map.cpp`). Touch control and
controller work at the same time; the controller only sends when something on it is moved.

## Bluetooth on the robot

The ESP32-S3 only supports **Bluetooth Low Energy** (BLE), not classic Bluetooth. Series X|S Xbox controllers
support this out of the box; the **Elite Series 2** (also “Core”) and Xbox One controllers with Bluetooth only
with current firmware (since late 2021). So open the **Xbox Accessories** app (Windows/Xbox) once beforehand
and update the controller.

1. *Controller* → switch **Bluetooth on the robot** on → **Pair** (searches for 60 s).
2. Switch the controller on, then hold the **pair button** (top, next to the USB port) until the
   Xbox logo flashes fast.
3. After a few seconds: *connected: Xbox Wireless Controller · Battery …*, the controller vibrates briefly.

After that, switching the controller on is enough – the robot searches for it with little radio load and
connects automatically. **Forget** deletes the pairing.

Notes:

- An Xbox controller remembers **one** Bluetooth device (plus one Xbox). After pairing with the robot,
  it has to be paired again with the phone/PC and vice versa (double-tapping the pair button switches
  between the most recently paired devices).
- Bluetooth and Wi-Fi share the antenna. While Bluetooth is on, the Wi-Fi therefore runs in
  power-saving mode (modem sleep, mandated by the ESP32 for coexistence): response time and
  camera stream become somewhat more sluggish. The connection to the controller runs at a 15–30 ms interval; the
  robot rejects the controller's request for 7.5 ms, otherwise there would be hardly any airtime left for the Wi-Fi. If Bluetooth is off, it is not started at all (no memory, no
  airtime) and the Wi-Fi runs without power saving again – the camera stream is then at its smoothest.
- If pairing fails, the **Events** log (*System*) shows whether any Bluetooth devices or an
  Xbox controller were seen at all. “No Xbox controller found” usually means: controller firmware too old
  or the pair button not held long enough. On connecting, it also lists the interval and free memory,
  and on a disconnect, the reason. If the Wi-Fi drops out while the controller is connected: switch the controller off – the
  robot then only searches occasionally and the Wi-Fi comes back.
- If the connection drops (controller off, out of range), the robot stops after approx. 2 s at the latest
  (Bluetooth timeout; the controller only reports changes, so a held stick remains in effect until then).
- Not yet tested with real hardware: the controller's report is evaluated as in the library
  [XboxSeriesXControllerESP32](https://github.com/asukiaaa/arduino-XboxSeriesXControllerESP32)
  (Bluetooth stack: [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.5.1). If the sticks
  move the wrong way round in the graphic, *Invert sticks up/down* helps.

## Controller on this device

Switch **Controller on this device** on (applies to this browser). Connect the controller to the
phone/PC via Bluetooth or USB and press a button – only then does the browser report it. The page sends the
state to the robot every 50 ms while it is open (regardless of which sub-page). The browser usually does not report the Xbox button and the
Elite paddles.

## Mapping

At the top of the *Controller* page the controller is shown large, each button with its function:

- **Change:** tap a button or label → choose a function (sorted by topic; “also on B” shows
  where a function is already assigned – several buttons may have the same one). Saved immediately;
  *Default* resets everything.
- **Live:** if a controller is active (Bluetooth or browser), pressed buttons light up together with their label,
  the sticks move along and the triggers fill up. If the mode changes on the controller, the graphic follows.
- **Sticks and triggers** are fixed per mode (dashed boxes); switch between *Walk*, *Body* and
  *Front legs* at the top right to see their function. *Swap sticks* (*Sticks* card) puts driving
  on the right stick (e.g. for left-handers), in all modes.
- On a phone, a tappable list appears below the graphic.

The sticks depend on the **mode** (button Y changes it; the control panel shows it as a 🎮 badge):

| Mode | Left stick | Right stick | Triggers |
|---|---|---|---|
| Walk | drive (forward/back, sideways) | turn | RT faster up to 100 %, LT slower |
| Body | shift | tilt (roll/pitch) | LT/RT turn |
| Front legs | left leg (foot follows the stick) | right leg | RT extend, LT pull in |

Switching to *Front legs* mode raises them (while standing); any other mode lowers them again.

Buttons (default, freely changeable in the graphic):

| Button | Action |
|---|---|
| A | Stand up / sit down (servos off: switch them on first, then press again) |
| B | Stop (also ends a gesture) |
| X | Raise / lower front legs |
| Y | Change mode (Walk → Body → Front legs) |
| LB / RB | Previous / next gait |
| D-pad ↑ ↓ | Body higher / lower (5 mm, holding repeats) |
| D-pad ← → | Speed − / + (10 %) |
| View | Alive on/off |
| Menu | Next gesture (Circle, Yes, No, Push-up, Dance) |
| Xbox | **E-stop** (release only in the UI) |
| LS / RS (press) | Levelling on/off / Wave |
| Share | Reset posture |
| P1 / P2 / P3 / P4 | Mode Walk / Body / Front legs / Stop (Elite paddles) |

Further selectable actions: mode directly, individual gestures, servos on/off. A button still
pressed when connecting (e.g. the Xbox button used to switch on) triggers nothing.

**Sticks:** *Speed* is the base speed when walking, *Dead zone* the deflection that is ignored (against
stick drift), *Fine control around the centre* makes small deflections finer (response curve) without changing the
maximum.

Controller messages (mode, gait, errors such as “Stand up first”) appear in the UI.

## Tests

[`test_host/pad_test.cpp`](../firmware/test_host/pad_test.cpp): evaluation of the Bluetooth report (sticks,
triggers, buttons, D-pad, Elite paddles), dead zone and response curve, directions per mode, edges and
repetition, mode change, mapping.
