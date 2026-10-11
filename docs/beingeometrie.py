#!/usr/bin/env python3
"""Erzeugt docs/beingeometrie.svg (Maßzeichnung Bein + Körper).

Maße unten ändern und neu erzeugen:
    python3 docs/beingeometrie.py
Die Werte müssen zu firmware/include/robot_geometry.h passen.
PNG-Vorschau z. B. mit Chromium:
    chromium --headless --screenshot=docs/beingeometrie.png --window-size=1600,1130 docs/beingeometrie.svg
"""

import math
from pathlib import Path

# --- Maße in mm (Achse zu Achse) -------------------------------------------
COXA, FEMUR, TIBIA = 41.0, 43.0, 94.0
FEMUR_NEUTRAL_DEG = 15.0  # Montagestellung: Femur bei Servo 90° über der Waagerechten, Tibia senkrecht
FRONT_SPAN, MID_SPAN, LENGTH = 77.0, 114.0, 147.0  # Abstände der Coxa-Achsen
CORNER_ANGLE = 45.0  # Beine vorne/hinten gegenüber der Längsachse

# Körperkontur nach CAD (nur Darstellung)
PLATE_X, PLATE_Y = 72.0, 43.5  # halbe Länge/Breite der Grundplatte
ESP_X0, ESP_X1, ESP_Y = -27.0, 60.0, 21.0  # ESP32-Platine

# --- abgeleitete Werte ------------------------------------------------------
fa = math.radians(FEMUR_NEUTRAL_DEG)
REACH = COXA + FEMUR * math.cos(fa)  # Coxa-Achse -> Fuß (waagerecht)
HEIGHT = TIBIA - FEMUR * math.sin(fa)  # Coxa-Achse über Boden
MOUNTS = [  # (Name, x, y, Winkel, PCA, Kanäle)
    ("vorne links", LENGTH / 2, FRONT_SPAN / 2, CORNER_ANGLE, "0x40", "CH0–2"),
    ("Mitte links", 0.0, MID_SPAN / 2, 90.0, "0x40", "CH3–5"),
    ("hinten links", -LENGTH / 2, FRONT_SPAN / 2, 180.0 - CORNER_ANGLE, "0x40", "CH6–8"),
    ("vorne rechts", LENGTH / 2, -FRONT_SPAN / 2, -CORNER_ANGLE, "0x41", "CH0–2"),
    ("Mitte rechts", 0.0, -MID_SPAN / 2, -90.0, "0x41", "CH3–5"),
    ("hinten rechts", -LENGTH / 2, -FRONT_SPAN / 2, -(180.0 - CORNER_ANGLE), "0x41", "CH6–8"),
]

# --- Darstellung ------------------------------------------------------------
W, H = 1600, 1130
FONT = "Helvetica, Arial, 'Liberation Sans', 'DejaVu Sans', sans-serif"
C_DIM = "#1f6fd1"
C_TEXT = "#222"
C_MUTED = "#667780"
C_LINE = "#333"
C_BODY = "#d3dbe0"
C_COXA = "#98a7b0"
C_FEMUR = "#45b0a1"
C_TIBIA = "#7a84d0"
C_ESP = "#4f8f6a"
C_AXIS = "#d0453f"

out = []


def fmt(v):
    """Zahl im deutschen Format, ohne überflüssige Nachkommastellen."""
    s = f"{v:.1f}".rstrip("0").rstrip(".")
    return s.replace(".", ",").replace("-", "−")


def el(s):
    out.append(s)


def text(x, y, s, size=14, anchor="middle", color=C_TEXT, weight="normal", extra=""):
    el(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" text-anchor="{anchor}" '
       f'fill="{color}" font-weight="{weight}" {extra}>{s}</text>')


def line(x1, y1, x2, y2, color=C_LINE, w=1.5, extra=""):
    el(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" '
       f'stroke-width="{w}" {extra}/>')


def joint(x, y, r=7):
    el(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="#fff" stroke="{C_LINE}" stroke-width="2"/>')


def label_box(x, y, s, size=17, color=C_DIM, angle=0):
    """Maßzahl mit weißem Hintergrund, zentriert auf (x, y)."""
    w = 0.6 * size * len(s) + 8
    h = size + 6
    rot = f' transform="rotate({angle:.1f} {x:.1f} {y:.1f})"' if angle else ""
    el(f'<g{rot}><rect x="{x - w / 2:.1f}" y="{y - h / 2:.1f}" width="{w:.1f}" height="{h}" '
       f'fill="#fff" rx="3"/>')
    text(x, y + size * 0.36, s, size=size, color=color, weight="bold")
    el("</g>")


def dim(x1, y1, x2, y2, s, offset=0.0, size=17, t=0.5):
    """Maßlinie zwischen zwei Punkten, parallel um 'offset' px verschoben."""
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    nx, ny = -uy, ux  # Normale (links der Richtung)
    ax, ay = x1 + nx * offset, y1 + ny * offset
    bx, by = x2 + nx * offset, y2 + ny * offset
    if offset:
        for (px, py), (qx, qy) in (((x1, y1), (ax, ay)), ((x2, y2), (bx, by))):
            sx, sy = px + nx * math.copysign(6, offset), py + ny * math.copysign(6, offset)
            ex, ey = qx + nx * math.copysign(6, offset), qy + ny * math.copysign(6, offset)
            line(sx, sy, ex, ey, C_DIM, 1)
    line(ax, ay, bx, by, C_DIM, 1.3, 'marker-start="url(#arr)" marker-end="url(#arr)"')
    ang = math.degrees(math.atan2(dy, dx))
    if ang > 90 or ang < -90:
        ang += 180
    label_box(ax + (bx - ax) * t, ay + (by - ay) * t, s, size=size, angle=ang)


def arc(cx, cy, r, a0, a1, color=C_DIM, w=1.3, arrow=False):
    """Kreisbogen in Bildschirmwinkeln (Grad, 0 = rechts, positiv = im Uhrzeigersinn)."""
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    sweep = 1 if a1 > a0 else 0
    large = 1 if abs(a1 - a0) > 180 else 0
    mk = 'marker-end="url(#arr)"' if arrow else ""
    el(f'<path d="M{p0[0]:.1f},{p0[1]:.1f} A{r},{r} 0 {large},{sweep} {p1[0]:.1f},{p1[1]:.1f}" '
       f'fill="none" stroke="{color}" stroke-width="{w}" {mk}/>')


# ============================================================================
el(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
   f'font-family="{FONT}">')
el('<defs><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" '
   f'orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" fill="{C_DIM}"/></marker>'
   '<pattern id="hatch" width="12" height="12" patternUnits="userSpaceOnUse" '
   'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="12" stroke="#8a969c" '
   'stroke-width="1.2"/></pattern></defs>')
el(f'<rect width="{W}" height="{H}" fill="#fff"/>')
el(f'<rect x="16" y="16" width="{W - 32}" height="{H - 32}" fill="none" stroke="{C_LINE}" stroke-width="1.5"/>')
text(40, 58, "Hexapod MK3 – Beingeometrie", size=28, anchor="start", weight="bold")
text(40, 84, "Maße aus dem CAD-Modell. Achsabstände (Drehpunkt zu Drehpunkt) in mm. "
     "Montagestellung = alle Servos auf 90° (1500 µs).", size=15, anchor="start", color=C_MUTED)

# ============================================================================
# Seitenansicht Bein
S1 = 4.0  # px/mm
X0, Y0 = 250.0, 310.0  # Coxa-Achse
GROUND = Y0 + HEIGHT * S1
fx, fy = X0 + COXA * S1, Y0  # Femur-Achse
kx, ky = fx + FEMUR * math.cos(fa) * S1, fy - FEMUR * math.sin(fa) * S1  # Knie
footx, footy = kx, GROUND

text(420, 130, "Bein – Seitenansicht (Montagestellung)", size=18, weight="bold")

# Boden
el(f'<rect x="60" y="{GROUND:.1f}" width="680" height="14" fill="url(#hatch)"/>')
line(60, GROUND, 740, GROUND, C_LINE, 2)

# Körper (Schnitt) mit Coxa-Servo-Achse
el(f'<rect x="{X0 - 120:.1f}" y="{Y0 - 45:.1f}" width="120" height="105" rx="6" fill="{C_BODY}" '
   f'stroke="{C_LINE}" stroke-width="1.5"/>')
text(X0 - 60, Y0 + 85, "Körper", size=14, color=C_MUTED)
line(X0, Y0 - 95, X0, GROUND + 18, C_AXIS, 1.2, 'stroke-dasharray="10 4 2 4"')
text(X0 + 6, Y0 - 100, "Coxa-Achse (senkrecht)", size=13, anchor="start", color=C_AXIS)

# Glieder
line(X0, Y0, fx, fy, C_COXA, 11, 'stroke-linecap="round"')
line(fx, fy, kx, ky, C_FEMUR, 11, 'stroke-linecap="round"')
line(kx, ky, footx, footy, C_TIBIA, 9, 'stroke-linecap="round"')
joint(X0, Y0)
joint(fx, fy)
joint(kx, ky)
el(f'<circle cx="{footx:.1f}" cy="{footy:.1f}" r="6" fill="{C_LINE}"/>')
text((X0 + fx) / 2, Y0 + 30, "Coxa", size=14, weight="bold")
text((fx + kx) / 2 - 42, (fy + ky) / 2 - 20, "Femur", size=14, weight="bold")
text(kx + 14, (ky + GROUND) / 2 + 60, "Tibia", size=14, anchor="start", weight="bold")
text(kx + 16, GROUND - 52, "Tibia = Linie Knieachse → Fußspitze", size=12, anchor="start", color=C_MUTED)
text(kx + 16, GROUND - 36, "(das Bauteil selbst ist gebogen)", size=12, anchor="start", color=C_MUTED)

# Maße der Glieder
dim(X0, Y0, fx, fy, fmt(COXA), offset=-48)
dim(fx, fy, kx, ky, fmt(FEMUR), offset=-52)
dim(kx, ky, footx, footy, fmt(TIBIA), offset=45)

# Femur-Winkel
line(fx, fy, fx + 110, fy, C_DIM, 1, 'stroke-dasharray="6 4"')
arc(fx, fy, 72, -FEMUR_NEUTRAL_DEG, 0)
text(fx + 78, fy - 4, f"{fmt(FEMUR_NEUTRAL_DEG)}°", size=15, anchor="start", color=C_DIM, weight="bold")

# Tibia-Winkel gegenüber "rechtwinklig zum Femur"
pdx, pdy = math.sin(fa), math.cos(fa)  # Bildschirmrichtung senkrecht zum Femur, nach unten/außen
line(kx, ky, kx + pdx * 120, ky + pdy * 120, C_DIM, 1, 'stroke-dasharray="6 4"')
arc(kx, ky, 95, 90 - FEMUR_NEUTRAL_DEG, 90)
text(kx + 42, ky + 96, f"{fmt(-FEMUR_NEUTRAL_DEG)}°", size=15, anchor="start", color=C_DIM, weight="bold")
text(kx + 42, ky + 114, "Tibia ggü. rechtwinklig", size=12, anchor="start", color=C_DIM)
text(kx + 42, ky + 129, "zum Femur", size=12, anchor="start", color=C_DIM)

# Höhe und Fußabstand
dim(X0 - 165, Y0, X0 - 165, GROUND, fmt(HEIGHT), offset=0)
line(X0 - 175, Y0, X0 - 125, Y0, C_DIM, 1)
text(X0 - 165, GROUND + 38, "Höhe Coxa-Achse", size=12, color=C_DIM)
text(X0 - 165, GROUND + 53, "über Boden", size=12, color=C_DIM)
yd = GROUND + 62
for xx in (X0, fx, footx):
    line(xx, GROUND + 18, xx, yd + 8, C_DIM, 1)
dim(X0, yd, footx, yd, fmt(REACH), offset=0)
dim(X0, yd + 40, fx, yd + 40, fmt(COXA), offset=0, size=14)
dim(fx, yd + 40, footx, yd + 40, fmt(FEMUR * math.cos(fa)), offset=0, size=14)
line(X0, yd + 8, X0, yd + 48, C_DIM, 1)
line(fx, yd + 8, fx, yd + 48, C_DIM, 1)
line(footx, yd + 8, footx, yd + 48, C_DIM, 1)
text((X0 + footx) / 2, yd + 82, "Fußabstand Coxa-Achse → Fuß (waagerecht)", size=12, color=C_DIM)

# ============================================================================
# Draufsicht Körper
S2 = 2.05
CX, CY = 1190.0, 458.0


def tp(x, y):
    """Roboterkoordinaten (x vorne, y links, mm) -> Bild."""
    return CX - y * S2, CY - x * S2


text(CX, 130, "Körper – Draufsicht (vorne = oben)", size=18, weight="bold")

# Kontur: erst alle Teile mit Rand, dann gefüllt darüber -> nur Außenkontur sichtbar
shapes = []
x0, y0 = tp(PLATE_X, PLATE_Y)
shapes.append(("rect", x0, y0, 2 * PLATE_Y * S2, 2 * PLATE_X * S2, 12))
for name, mx, my, a, *_ in MOUNTS:
    px, py = tp(mx, my)
    shapes.append(("circle", px, py, 10 * S2))
for my in (MID_SPAN / 2, -MID_SPAN / 2):  # seitliche Laschen der Mittelbeine
    ax, _ = tp(0, my)
    bx, _ = tp(0, math.copysign(PLATE_Y - 2, my))
    _, ty = tp(11, 0)
    shapes.append(("rect", min(ax, bx), ty, abs(ax - bx), 22 * S2, 0))
for layer in ("stroke", "fill"):
    attrs = (f'fill="{C_LINE}" stroke="{C_LINE}" stroke-width="3"' if layer == "stroke"
             else f'fill="{C_BODY}"')
    for s in shapes:
        if s[0] == "circle":
            el(f'<circle cx="{s[1]:.1f}" cy="{s[2]:.1f}" r="{s[3]:.1f}" {attrs}/>')
        else:
            el(f'<rect x="{s[1]:.1f}" y="{s[2]:.1f}" width="{s[3]:.1f}" height="{s[4]:.1f}" rx="{s[5]}" {attrs}/>')

# ESP32-Platine und Front
ex0, ey0 = tp(ESP_X1, ESP_Y)
el(f'<rect x="{ex0:.1f}" y="{ey0:.1f}" width="{2 * ESP_Y * S2:.1f}" height="{(ESP_X1 - ESP_X0) * S2:.1f}" '
   f'rx="4" fill="{C_ESP}" opacity="0.85"/>')
ecx, ecy = tp(ESP_X1 - 16, 0)
text(ecx, ecy - 4, "ESP32-S3", size=12, color="#fff", weight="bold")
text(ecx, ecy + 12, "CAM", size=12, color="#fff", weight="bold")
for yy in (12, -12):
    px, py = tp(PLATE_X + 2, yy)
    el(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="7" fill="#d23b33" stroke="{C_LINE}" stroke-width="1"/>')

# Längs-/Querachse
line(CX, tp(PLATE_X + 25, 0)[1], CX, tp(-PLATE_X - 25, 0)[1], C_AXIS, 1, 'stroke-dasharray="10 4 2 4"')
line(tp(0, MID_SPAN / 2 + 85)[0], CY, tp(0, -MID_SPAN / 2 - 85)[0], CY, C_AXIS, 1, 'stroke-dasharray="10 4 2 4"')

# Beine (Montagestellung, Fuß senkrecht unter dem Knie)
for i, (name, mx, my, a, pca, ch) in enumerate(MOUNTS):
    ar = math.radians(a)
    p0 = tp(mx, my)
    p1 = tp(mx + COXA * math.cos(ar), my + COXA * math.sin(ar))
    p2 = tp(mx + REACH * math.cos(ar), my + REACH * math.sin(ar))
    line(*p0, *p1, C_COXA, 9, 'stroke-linecap="round"')
    line(*p1, *p2, C_FEMUR, 9, 'stroke-linecap="round"')
    joint(*p0, r=6)
    joint(*p1, r=5)
    el(f'<circle cx="{p2[0]:.1f}" cy="{p2[1]:.1f}" r="6" fill="{C_LINE}"/>')
    lx, ly = tp(mx + (REACH + 22) * math.cos(ar), my + (REACH + 22) * math.sin(ar))
    el(f'<circle cx="{lx:.1f}" cy="{ly:.1f}" r="13" fill="#fff" stroke="{C_LINE}" stroke-width="1.5"/>')
    text(lx, ly + 5, str(i + 1), size=15, weight="bold")
    left = my > 0
    tx = lx + (-20 if left else 20)
    ty = ly + (0 if abs(a) == 90 else (-2 if mx > 0 else 4))
    if abs(a) == 90:
        ty = ly + 34
        tx = lx
    anchor = "middle" if abs(a) == 90 else ("end" if left else "start")
    text(tx, ty, name, size=12, anchor=anchor, color=C_MUTED)
    text(tx, ty + 15, f"{pca} {ch}", size=12, anchor=anchor, color=C_MUTED)

# Montagewinkel
for name, mx, my, a, *_ in MOUNTS:
    if my > 0:
        continue
    px, py = tp(mx, my)
    ref = -90 if mx > 0 else 90  # Bildwinkel der Referenz: vorne bzw. hinten
    if mx == 0:
        continue
    line(px, py, px, py + (-75 if mx > 0 else 75), C_DIM, 1, 'stroke-dasharray="6 4"')
    a_img = ref + (CORNER_ANGLE if mx > 0 else -CORNER_ANGLE)
    arc(px, py, 58, min(ref, a_img), max(ref, a_img))
    mid = math.radians((ref + a_img) / 2)
    text(px + 78 * math.cos(mid), py + 78 * math.sin(mid) + 5, f"{fmt(CORNER_ANGLE)}°", size=14, color=C_DIM, weight="bold")
# Mitte rechts: rechter Winkel
px, py = tp(0, -MID_SPAN / 2)
el(f'<path d="M{px + 22:.1f},{py:.1f} L{px + 22:.1f},{py - 22:.1f} L{px:.1f},{py - 22:.1f}" fill="none" '
   f'stroke="{C_DIM}" stroke-width="1.2"/>')
text(px + 30, py - 28, "90°", size=14, anchor="start", color=C_DIM, weight="bold")

# Maße der Coxa-Achsen
fl, fr = tp(LENGTH / 2, FRONT_SPAN / 2), tp(LENGTH / 2, -FRONT_SPAN / 2)
dim(fl[0], fl[1], fr[0], fr[1], fmt(FRONT_SPAN), offset=-62)
ml, mr = tp(0, MID_SPAN / 2), tp(0, -MID_SPAN / 2)
rl = tp(-LENGTH / 2, MID_SPAN / 2)
yb = rl[1] + 95
for xx, y_from in ((ml[0], ml[1]), (mr[0], mr[1])):
    line(xx, y_from + 12, xx, yb + 8, C_DIM, 1)
dim(ml[0], yb, mr[0], yb, fmt(MID_SPAN))
xl = fl[0] - 70
for p in (fl, tp(-LENGTH / 2, FRONT_SPAN / 2)):
    line(p[0] - 12, p[1], xl - 8, p[1], C_DIM, 1)
dim(xl, fl[1], xl, tp(-LENGTH / 2, 0)[1], fmt(LENGTH), t=0.3)
# Fahrtrichtung und Koordinaten
ax_, ay_ = tp(PLATE_X + 70, 0)
el(f'<line x1="{ax_:.1f}" y1="{ay_ + 34:.1f}" x2="{ax_:.1f}" y2="{ay_:.1f}" stroke="{C_AXIS}" stroke-width="3" '
   f'marker-end="url(#arrR)"/>')
el(f'<defs><marker id="arrR" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="5" markerHeight="5" '
   f'orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{C_AXIS}"/></marker></defs>')
text(ax_ + 10, ay_ + 14, "Fahrtrichtung", size=13, anchor="start", color=C_AXIS, weight="bold")
el(f'<g transform="translate({CX + 2:.1f},{CY + 2:.1f})">'
   f'<line x1="0" y1="0" x2="0" y2="-34" stroke="{C_LINE}" stroke-width="1.5" marker-end="url(#arrK)"/>'
   f'<line x1="0" y1="0" x2="-34" y2="0" stroke="{C_LINE}" stroke-width="1.5" marker-end="url(#arrK)"/>'
   f'<text x="6" y="-26" font-size="12" fill="{C_LINE}">x</text>'
   f'<text x="-32" y="16" font-size="12" fill="{C_LINE}">y</text></g>')
el(f'<defs><marker id="arrK" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" '
   f'orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{C_LINE}"/></marker></defs>')

# ============================================================================
# Tabellen
TY = 818
BOX_H = 260
el(f'<rect x="40" y="{TY}" width="1520" height="{BOX_H}" fill="#f4f6f8" stroke="#c4ccd1"/>')

# Spalte 1: Montagepunkte
c1 = 60
text(c1, TY + 30, "Firmware: firmware/include/robot_geometry.h", size=15, anchor="start", weight="bold")
text(c1, TY + 54, f"LEG_GEOMETRY  coxa {fmt(COXA)} · femur {fmt(FEMUR)} · tibia {fmt(TIBIA)} mm",
     size=14, anchor="start")
cols = [c1, c1 + 150, c1 + 225, c1 + 295, c1 + 375]
hy = TY + 84
for x, h in zip(cols, ["Bein", "x", "y", "Winkel", "PCA9685"]):
    text(x, hy, h, size=13, anchor="start", color=C_MUTED, weight="bold")
line(c1, hy + 7, c1 + 500, hy + 7, "#c4ccd1", 1)
for i, (name, mx, my, a, pca, ch) in enumerate(MOUNTS):
    y = hy + 27 + i * 23
    vals = [f"{i + 1}  {name}", fmt(mx), fmt(my), f"{fmt(a)}°", f"{pca} {ch}"]
    for x, v in zip(cols, vals):
        text(x, y, v, size=14, anchor="start")
text(c1, TY + BOX_H - 14, "x vorne, y links (mm, ab Körpermitte) · Winkel ab vorne, links +",
     size=12, anchor="start", color=C_MUTED)

# Spalte 2: Montagestellung
c2 = 620
text(c2, TY + 30, "Montagestellung = alle Servos 90° (1500 µs)", size=15, anchor="start", weight="bold")
rows = [
    ("Gelenkwinkel (Firmware)", f"Coxa 0° · Femur +{fmt(FEMUR_NEUTRAL_DEG)}° · Tibia {fmt(-FEMUR_NEUTRAL_DEG)}°"),
    ("Körperhöhe (Coxa-Achse)", f"{fmt(HEIGHT)} mm über Boden"),
    ("Fußabstand", f"{fmt(REACH)} mm ab Coxa-Achse"),
    ("Fuß-Mitte (Mittelbeine)", f"{fmt(MID_SPAN / 2 + REACH)} mm ab Längsachse"),
]
for i, (k, v) in enumerate(rows):
    y = TY + 62 + i * 28
    text(c2, y, k, size=14, anchor="start", color=C_MUTED)
    text(c2 + 190, y, v, size=14, anchor="start")
text(c2, TY + 192, "Diese Haltung ist die Standardhaltung der IK", size=13, anchor="start", color=C_TEXT)
text(c2, TY + 210, "(Tab Steuerung → Körperhaltung).", size=13, anchor="start", color=C_TEXT)
text(c2, TY + BOX_H - 14, "Tibia senkrecht = 15° nach innen ggü. rechtwinklig zum Femur",
     size=12, anchor="start", color=C_MUTED)

# Spalte 3: Kalibrierung
c3 = 1130
text(c3, TY + 30, "Kalibrier-Nullpunkt (0°)", size=15, anchor="start", weight="bold")
rows = [
    ("Coxa", "Bein zeigt in Montagerichtung"),
    ("", f"(Ecken {fmt(CORNER_ANGLE)}°, Mitte 90°)"),
    ("Femur", "waagerecht (15° unter Montagestellung)"),
    ("Tibia", "rechtwinklig zum Femur –"),
    ("", "zuerst Femur kalibrieren"),
]
for i, (k, v) in enumerate(rows):
    y = TY + 62 + i * 24
    text(c3, y, k, size=14, anchor="start", color=C_MUTED, weight="bold")
    text(c3 + 60, y, v, size=14, anchor="start")
text(c3, TY + 196, "+ Richtung: Coxa Fuß nach vorne,", size=13, anchor="start")
text(c3, TY + 214, "Femur hoch, Tibia Fuß nach außen", size=13, anchor="start")
text(c3, TY + BOX_H - 14, "Details: docs/kalibrierung.md", size=12, anchor="start", color=C_MUTED)

text(40, H - 28, "Alle Längen sind Achsabstände (Drehpunkt zu Drehpunkt), Tibia = Knieachse → Fußspitze. "
     "Erzeugt mit docs/beingeometrie.py – Maße dort ändern und neu erzeugen.", size=12, anchor="start",
     color=C_MUTED)

el("</svg>")
Path(__file__).with_suffix(".svg").write_text("\n".join(out) + "\n", encoding="utf-8")
print(f"Reichweite {REACH:.2f} mm, Höhe {HEIGHT:.2f} mm")
