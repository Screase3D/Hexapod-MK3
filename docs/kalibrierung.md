# Servo Calibration

The inverse kinematics works in **joint angles**. Calibration defines, for each of the
18 servos, which pulse width corresponds to which angle. The reference point is the
**assembly pose** – the pose in which the servo horns are fitted with the servo centred (1500 µs):

```
puls_us = center_us + (winkel_deg − ref_deg) × us_per_deg     (clamped to min_deg … max_deg)
```

| Value | Meaning |
|---|---|
| `ref_deg` | Joint angle of the assembly pose, fixed per joint type (`ASSEMBLY_POSE` in `robot_geometry.h`): coxa 0°, femur +55.4°, tibia −33.8° |
| `center_us` | Pulse width in the assembly pose (≈ 1500 µs after fitting the horns) |
| `us_per_deg` | Slope; the **sign** is the direction of rotation (MG90S ≈ ±11.1 µs/°) |
| `min_deg`, `max_deg` | Software end stops of the joint (joint angles) |

The values are stored in the ESP32's NVS (they survive restarts and firmware updates) and can be
backed up as JSON with **Export** in the **Calibration & fine-tuning** section – ideally, put a copy in the
repository.

## Assembly pose

The horns are not fitted at 0° (femur horizontal) but in the **default posture**
in which the robot stands (height jacked up as in the CAD model, feet 100 mm from the coxa axis):

| Joint | Assembly pose | Joint angle |
|---|---|---|
| Coxa | Leg points in the mounting direction (corners 45°, middle 90°) | 0° |
| Femur | 55.4° upward | +55.4° |
| Tibia | Line knee → foot tip 68° below horizontal, foot tip 35 mm outside the knee | −33.8° |

In practice this means: **jack the robot up so that the femur axes are 52 mm above the table**
(underside of the chassis approx. 28 mm) and place the foot tips on the table **100 mm from the coxa axis**
– then every leg is exactly in the assembly pose.

This puts the servo centre in the middle of the working range: walking and sitting stay within ±50°;
only the body at its highest setting needs −65° at the femur (see [kinematik.md](kinematik.md)). The assembly pose is also the **default posture**
of the IK – “All to assembly pose” makes the robot stand.

For fitting and checking there is a **printable jig**:
[montagelehre.pdf](montagelehre.pdf) (A4, print at 100 %, measure the 50 mm check dimension).
It shows the axis lines at full scale: femur axis and knee axis as crosses, the
foot tip as a mark, the curved lower leg as an outline, and an angle gauge for the coxa.
If you want more precision, design small wedges in CAD that set exactly 55.4° between coxa bracket and femur
and 56.2° (knee angle between the axis lines) between femur and tibia.

## Joint convention

Applies **identically to both sides of the body**, so that the same numbers mean the same thing on the left and right.

### Zero position (all joints 0°)

The angle convention is pure mathematics; the assembly pose above has different angles.

Top view (front = up) – each coxa points straight away from the body along its mounting axis:

```
            ▲ FRONT
   Leg 1 ───┐     ┌── Leg 4
            │     │
   Leg 2 ───┤     ├── Leg 5
            │     │
   Leg 3 ───┘     └── Leg 6
```

Side view of a leg – femur horizontal, tibia at a right angle to it (pointing straight down):

```
     Body ┃
          ┃═●━━━━━━━━━━━●   ● = joint axes (femur joint, knee)
          ┃   Femur     ┃
                        ┃ Tibia
                        ┃
                        ▼ Foot
```

### Positive direction of rotation

| Joint | 0° | positive angle |
|---|---|---|
| **Coxa** | Leg points away from the body along the mounting axis | Foot swings **forward** |
| **Femur** | Upper leg horizontal | Upper leg rises (**upward**) |
| **Tibia** | Lower leg at 90° to the femur (pointing straight down when the femur is horizontal) | Foot moves **outward** (knee opens) |

“Tibia” means the line from the knee joint axis to the foot tip – with curved lower legs,
that is not the outer edge of the part.

### Default direction of rotation and quick direction test

The legs on the left and right are built as **mirror images**, so on the left every joint turns
the other way for the same pulse (femur/tibia: servos mounted mirrored; coxa: all shafts point
up, but “foot forward” is clockwise on the left and counter-clockwise on the right). Without the
“direction” calibration step, the defaults are therefore +11.1 µs/° on the right and −11.1 µs/° on the left (`SERVO_DEFAULT_DIR` in
`src/servo/servo_map.h`).

Check before the robot stands or walks (torque on, robot jacked up): under
*Calibration & fine-tuning* → **Fine-tune**, move each leg's slider a little from the assembly pose
to the **right** (larger angle):

| Joint | correct if … |
|---|---|
| Coxa | the foot swings **forward** (towards the camera for legs 1 and 4) |
| Femur | the upper leg goes **up** |
| Tibia | the foot tip goes **outward** |

If a joint moves the other way: under **Calibrate**, select the servo,
press **“Reverse direction”**, and finally **Save**. The assembly pose is preserved.
Typical symptom when an entire side turns the wrong way: when sitting down/standing up it moves
mirror-inverted, and when walking the robot makes no forward progress.

## Procedure

> Before powering on for the first time: jack the robot up so that the legs hang freely.
> Emergency stop: red **STOP** button or the **Esc** key.

1. **Fit the horns.** Remove the servo horns. Under *Calibration & fine-tuning*, switch **Torque**
   on and press **“All to 1500 µs”**. Now fit the horns/leg parts so that each
   leg is as close as possible to the assembly pose (mounting jig). The spline only allows
   steps of one tooth (≈ 18°) – so choose the tooth with the smallest
   deviation; step 2 takes care of the rest.
2. **Fine-tune the assembly pose.** Click a joint in the leg overview, move it exactly into the assembly pose in raw mode
   with ±1/±10 µs (or the arrow keys ←/→, with Shift ±10),
   and press **“Current position = assembly pose”**. For each leg, do **the femur first**, then
   the tibia (the jig is aligned to the femur and knee axes).
3. **Direction and scale.** Move the joint to a second known angle, enter the angle
   **with the sign according to the table above**, and press **“Current position = reference”**.
   This yields `us_per_deg` including the direction of rotation. Easy to measure are:
   - Coxa ±45°: the corner legs are then parallel or perpendicular to the body's longitudinal axis, the middle legs
     parallel to the corner legs in the assembly pose (angle gauge C),
   - Femur 0°: horizontal (red line on the jig, spirit-level app),
   - Tibia 0°: perpendicular to the femur (blue line on the jig).

   If you skip this step, check the direction with the test slider and, if necessary, use
   **“Reverse direction”** – the assembly pose is preserved.
4. **Limits.** With the test slider (section 3, calibrated), move slowly until just before
   mechanical collisions and enter `Min`/`Max` → **“Apply values”**.
5. Repeat for all 18 servos (↑/↓ jumps to the previous/next servo). Calibrated
   joints get a green dot in the overview.
6. **Save** – only then are the values stored permanently in NVS. “Discard” reloads the last
   saved state, “Defaults” resets everything (also only permanent after Save).
7. Press **Export** at the top of the page and keep the JSON file safe.

## Notes

- **Fine-tune** toggle (same page): all 18 joints calibrated at a glance, one
  slider each – handy for checking all legs together after calibration.
- **Raw mode ignores the limits** (only 500–2500 µs absolute). Move slowly while the
  legs are mounted.
- All movements go through a ramp (default 240 °/s, adjustable under *Settings* → *Servos*).
- After power-up, the servos move straight to the
  sitting pose when torque is enabled (feet far out, see [kinematik.md](kinematik.md#aufstehen-und-hinsetzen); the
  actual position is unknown beforehand). For assembly, then use “All to 1500 µs”.
- Importing a JSON file overwrites all 18 servos and saves immediately.
- Calibrations from older firmware versions (reference 0°, version 1) are automatically converted to the assembly pose
  on load and on import; servos that were never calibrated (1500 µs,
  default slope) remain at default values. Recalibrate anyway after re-fitting the horns.
- If `ASSEMBLY_POSE` in `robot_geometry.h` is changed, the horns must be realigned and
  step 2 repeated.

## IMU

The MPU6050 provides pitch and roll with the same signs as the body posture:
**pitch + = nose up**, **roll + = right side down**. The settings are stored in NVS.

1. **Mounting orientation** (*Camera & IMU* page): direction of the X arrow on the GY-521 board
   (front/left/rear/right) and component side (top/bottom). Check under *Control* → *Attitude*:
   lift the nose → pitch becomes positive, lower the right side → roll becomes positive. Otherwise change the mounting
   orientation. (A change clears the zero point.)
2. **Zero point:** place the robot on a level surface (standing or jacked up) and
   press **“Set zero point (level)”**; do not touch it for approx. 2 s. This compensates for a board
   mounted at an angle; the display then shows 0°/0°.
3. **Gyro zero:** measured at every start – but only applied if the robot was
   standing still. If it is moving (e.g. held in the hand), the value last saved during a calibration
   is used. When stationary, it is also slowly tracked (temperature drift).
   “Recalibrate gyro” measures it again immediately (hold still for 2 s).

If the robot wobbles during the measurement, it aborts with a message – simply repeat it.
A wrong gyro zero shows up as a fixed angle error of about 1° per 1 °/s.
