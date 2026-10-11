#!/usr/bin/env python3
"""Erzeugt docs/montagelehre.svg: Lehre im Maßstab 1:1 (A4) zum Aufstecken der Servohörner.

    python3 docs/montagelehre.py
PDF (zum Drucken mit 100 % / "Tatsächliche Größe"):
    chromium --headless --no-pdf-header-footer --print-to-pdf=docs/montagelehre.pdf docs/montagelehre.svg
Die Werte müssen zu firmware/include/robot_geometry.h (LEG_GEOMETRY, ASSEMBLY_POSE) passen.
"""

import math
from pathlib import Path

COXA, FEMUR, TIBIA = 41.0, 43.0, 94.0
FEMUR_DEG = 30.0  # Montagestellung: Femur über der Waagerechten
TIBIA_DEG = -30.0  # Montagestellung: Tibia ggü. "rechtwinklig zum Femur" (hier: senkrecht)
CORNER_ANGLE = 45.0

FONT = "Helvetica, Arial, 'Liberation Sans', 'DejaVu Sans', sans-serif"
C_LINE, C_REF, C_MUTED, C_DIM = "#222", "#d0453f", "#666", "#1f6fd1"

out = []
el = out.append


def fmt(v):
    s = f"{v:.1f}".rstrip("0").rstrip(".")
    return s.replace(".", ",").replace("-", "−")


def text(x, y, s, size=3.2, anchor="start", color=C_LINE, weight="normal", extra=""):
    el(f'<text x="{x:.2f}" y="{y:.2f}" font-size="{size}" text-anchor="{anchor}" fill="{color}" '
       f'font-weight="{weight}" {extra}>{s}</text>')


def line(x1, y1, x2, y2, color=C_LINE, w=0.35, extra=""):
    el(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="{color}" '
       f'stroke-width="{w}" {extra}/>')


def cross(x, y, label, lx, ly, anchor="start"):
    el(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3" fill="none" stroke="{C_LINE}" stroke-width="0.35"/>')
    line(x - 5, y, x + 5, y, C_LINE, 0.25)
    line(x, y - 5, x, y + 5, C_LINE, 0.25)
    el(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="0.5" fill="{C_LINE}"/>')
    text(lx, ly, label, 2.8, anchor, C_LINE, "bold")


def arc(cx, cy, r, a0, a1, color=C_DIM):
    """Bogen in Bildwinkeln (Grad, 0 = rechts, im Uhrzeigersinn)."""
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    el(f'<path d="M{p0[0]:.2f},{p0[1]:.2f} A{r},{r} 0 0,{1 if a1 > a0 else 0} {p1[0]:.2f},{p1[1]:.2f}" '
       f'fill="none" stroke="{color}" stroke-width="0.3"/>')


def leg_template(ox, oy, mirror, title):
    """Seitenansicht eines Beins; (ox, oy) = Femur-Achse. mirror: Bein zeigt nach links."""
    sx = -1 if mirror else 1
    a_start, a_end = ("end", "start") if mirror else ("start", "end")
    fa = math.radians(FEMUR_DEG)
    kx, ky = ox + sx * FEMUR * math.cos(fa), oy - FEMUR * math.sin(fa)
    td = math.radians(FEMUR_DEG + TIBIA_DEG + 90 - 180)  # Tibia-Richtung ggü. Waagerechte
    fx, fy = kx + sx * TIBIA * math.cos(td), ky - TIBIA * math.sin(td)
    inner, outer = ox - sx * 12, kx + sx * 25

    # Umriss zum Ausschneiden
    x0, x1 = sorted([inner, outer])
    el(f'<rect x="{x0:.2f}" y="{ky - 14:.2f}" width="{x1 - x0:.2f}" height="{fy - ky + 24:.2f}" '
       f'fill="none" stroke="#999" stroke-width="0.25" stroke-dasharray="2 1.5"/>')
    text((x0 + x1) / 2, ky - 8, title, 3.4, "middle", weight="bold")

    # Körperebene (waagerecht = Femur 0°)
    line(inner, oy, kx + sx * 12, oy, C_REF, 0.3, 'stroke-dasharray="3 1.2"')
    text(ox + sx * 7, oy + 4, "waagerecht (Femur 0°)", 2.3, a_start, C_REF)
    text(inner + sx * 1, oy - 1.5, "Körper", 2.3, a_start, C_REF)

    # Boden in der Standardhaltung
    line(x0, fy, x1, fy, C_MUTED, 0.3)
    text((x0 + x1) / 2, fy + 4, f"Boden in Standardhaltung ({fmt(fy - oy)} mm unter der Achse)", 2.3, "middle", C_MUTED)

    # Glieder (Achslinien)
    line(ox, oy, kx, ky, C_LINE, 0.6)
    line(kx, ky, fx, fy, C_LINE, 0.6)
    mx, my = (ox + kx) / 2 - sx * 3 * math.sin(fa), (oy + ky) / 2 - 3 * math.cos(fa)
    text(mx, my, f"Femur {fmt(FEMUR)}", 2.6, "middle", extra=f'transform="rotate({-sx * FEMUR_DEG:.1f} {mx:.2f} {my:.2f})"')
    text(kx + sx * 2, (ky + fy) / 2 + 10, f"Tibia {fmt(TIBIA)}", 2.6, a_start)

    # Tibia 0° (rechtwinklig zum Femur) als Bezug für den zweiten Kalibrierpunkt
    ex, ey = kx + sx * math.sin(fa) * 34, ky + math.cos(fa) * 34
    line(kx, ky, ex, ey, C_DIM, 0.25, 'stroke-dasharray="2 1"')
    text(ex - sx * 4, ey + 3.5, "Tibia 0° (⟂ Femur)", 2.3, "middle", C_DIM)

    # Femur-Winkel
    if mirror:
        arc(ox, oy, 12, 180, 180 + FEMUR_DEG)
    else:
        arc(ox, oy, 12, -FEMUR_DEG, 0)
    text(ox + sx * 13.5, oy - 1.8, f"{fmt(FEMUR_DEG)}°", 2.8, a_start, C_DIM, "bold")

    cross(ox, oy, "Femur-Achse", ox, oy + 9, "middle")
    cross(kx, ky, "Knie-Achse", kx - sx * 6, ky - 3.5, a_end)
    # Fußspitze
    el(f'<path d="M{fx - 3:.2f},{fy - 5:.2f} L{fx:.2f},{fy:.2f} L{fx + 3:.2f},{fy - 5:.2f}" fill="none" '
       f'stroke="{C_LINE}" stroke-width="0.4"/>')
    el(f'<circle cx="{fx:.2f}" cy="{fy:.2f}" r="0.7" fill="{C_LINE}"/>')
    text(fx + sx * 4, fy - 2, "Fußspitze", 2.8, a_start, weight="bold")


el('<svg xmlns="http://www.w3.org/2000/svg" width="210mm" height="297mm" viewBox="0 0 210 297" '
   f'font-family="{FONT}">')
el('<rect width="210" height="297" fill="#fff"/>')
text(12, 16, "Hexapod MK3 – Montagelehre (1:1)", 5.5, weight="bold")
text(12, 22, f"Montagestellung bei 1500 µs: Coxa 0° · Femur +{fmt(FEMUR_DEG)}° · Tibia {fmt(TIBIA_DEG)}° (senkrecht). "
     "Mit 100 % / „Tatsächliche Größe“ drucken.", 2.8, color=C_MUTED)

# Kontrollmaß
y0 = 29
line(12, y0, 62, y0, C_LINE, 0.4)
for i in range(0, 51, 5):
    line(12 + i, y0 - (2 if i % 10 == 0 else 1.2), 12 + i, y0, C_LINE, 0.25)
text(65, y0 + 1, "Kontrollmaß 50 mm – nachmessen, sonst stimmt der Maßstab nicht", 2.8, color=C_REF, weight="bold")

leg_template(30, 80, False, "A – Bein zeigt nach rechts")
leg_template(180, 80, True, "B – Bein zeigt nach links")

# Coxa-Winkellehre (Draufsicht)
R = 55
gy = 240
gx = 20
text(gx - 8, gy - R - 8, "C – Coxa-Winkel (Draufsicht)", 3.4, weight="bold")
el(f'<path d="M{gx},{gy} L{gx + R},{gy} A{R},{R} 0 0,0 {gx},{gy - R} z" fill="none" stroke="#999" '
   'stroke-width="0.25" stroke-dasharray="2 1.5"/>')
for a in range(0, 91, 5):
    r0 = R - (5 if a % 15 == 0 else 2.5)
    ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
    line(gx + r0 * ca, gy - r0 * sa, gx + R * ca, gy - R * sa, C_LINE, 0.25)
for a, lab in ((0, "Körperlängsachse"), (CORNER_ANGLE, f"Eckbein {fmt(CORNER_ANGLE)}°"), (90, "Mittelbein 90°")):
    ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
    line(gx, gy, gx + (R - 6) * ca, gy - (R - 6) * sa, C_REF if a else C_LINE, 0.4)
el(f'<circle cx="{gx}" cy="{gy}" r="0.7" fill="{C_LINE}"/>')
text(gx + 3, gy + 4, "Coxa-Achse", 2.6, weight="bold")
text(gx + R, gy + 4, "parallel zur Längsachse", 2.4, "end", color=C_MUTED)
text(gx + 0.68 * R + 2, gy - 0.72 * R, f"Eckbeine {fmt(CORNER_ANGLE)}°", 2.6, color=C_REF, weight="bold")
text(gx - 1, gy - R - 2, "Mittelbeine 90°", 2.6, color=C_REF, weight="bold")

# Anleitung
tx, ty = 92, 182
steps = [
    ("So geht's", None),
    ("1.", "Roboter aufbocken (Beine frei). Oberfläche → Kalibrierung:"),
    ("", "Torque an, „Alle auf 1500 µs (Montage)“."),
    ("2.", "Lehre A/B ausschneiden, am besten auf Karton kleben;"),
    ("", "A oder B – je nachdem, von welcher Seite du schaust."),
    ("3.", "Kreuz „Femur-Achse“ auf die Femur-Drehachse legen, rote"),
    ("", "Linie parallel zur Körperebene (Unterseite/Coxa-Halter)."),
    ("4.", "Femur-Horn auf den Zahn stecken, bei dem die Knie-Achse"),
    ("", "am nächsten am Kreuz „Knie-Achse“ liegt (1 Zahn ≈ 18°)."),
    ("5.", "Tibia-Horn so, dass die Fußspitze auf der Marke liegt."),
    ("6.", "Coxa mit Lehre C: Bein in Montagerichtung (45° / 90°)."),
    ("7.", "Feinjustage je Servo im Rohmodus (±1 µs) bis alles"),
    ("", "deckungsgleich ist → „Aktuelle Stellung = Montagestellung“."),
    ("", "Je Bein zuerst Femur, dann Tibia. Details: docs/kalibrierung.md"),
]
y = ty
for k, v in steps:
    if v is None:
        text(tx, y, k, 3.4, weight="bold")
        y += 6
        continue
    text(tx, y, k, 2.8, weight="bold")
    text(tx + 5, y, v, 2.8)
    y += 4.6

text(12, 287, "Linien = Achslinien (Drehpunkt zu Drehpunkt), nicht Bauteilkanten. "
     "Erzeugt mit docs/montagelehre.py.", 2.4, color=C_MUTED)
el("</svg>")
Path(__file__).with_suffix(".svg").write_text("\n".join(out) + "\n", encoding="utf-8")
