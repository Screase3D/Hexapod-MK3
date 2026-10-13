"""Erzeugt die App-Icons der Weboberfläche (web/icon-*.png, web/icon.svg).

Sechseck mit zwei "Augen" auf dunklem Grund. Nur bei Änderungen am Aussehen neu
ausführen:  python tools/make_icons.py   (braucht Pillow)
"""

import math
import os

from PIL import Image, ImageDraw

BG = (13, 18, 23)
ACCENT = (63, 182, 168)
EYE = (13, 18, 23)
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")


def hexagon(cx, cy, r):
    return [(cx + r * math.sin(math.radians(a)), cy - r * math.cos(math.radians(a))) for a in range(0, 360, 60)]


def icon(size):
    s = 4  # überabgetastet, dann verkleinert (glatte Kanten)
    n = size * s
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    c = n / 2
    d.polygon(hexagon(c, c, n * 0.36), fill=ACCENT)
    for dx in (-1, 1):  # Augen
        ex, ey, er = c + dx * n * 0.1, c - n * 0.03, n * 0.055
        d.ellipse((ex - er, ey - er, ex + er, ey + er), fill=EYE)
    return img.resize((size, size), Image.LANCZOS)


def svg():
    pts = " ".join("%.1f,%.1f" % p for p in hexagon(50, 50, 36))
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
        '<rect width="100" height="100" fill="#0d1217"/>'
        '<polygon points="%s" fill="#3fb6a8"/>'
        '<circle cx="40" cy="47" r="5.5" fill="#0d1217"/><circle cx="60" cy="47" r="5.5" fill="#0d1217"/>'
        "</svg>\n" % pts
    )


if __name__ == "__main__":
    for size in (180, 192, 512):
        icon(size).save(os.path.join(WEB, "icon-%d.png" % size), optimize=True)
    with open(os.path.join(WEB, "icon.svg"), "w", encoding="utf-8") as f:
        f.write(svg())
    print("Icons erzeugt")
