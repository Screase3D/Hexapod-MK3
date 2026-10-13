"""PlatformIO pre-build script: packt alle Dateien aus web/ gzip-komprimiert in
src/generated/web_assets.h, damit die Oberfläche direkt aus dem Flash
ausgeliefert wird (kein separates Dateisystem-Image nötig).

Kann auch ohne PlatformIO aufgerufen werden:  python scripts/embed_web.py
"""

import gzip
import os
import sys

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".json": "application/json",
}


def project_dir():
    try:
        Import("env")  # noqa: F821  (nur unter PlatformIO definiert)
        return env.subst("$PROJECT_DIR")  # noqa: F821
    except NameError:
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def c_ident(path):
    return "asset_" + "".join(c if c.isalnum() else "_" for c in path.strip("/"))


def ui_version(root):
    sys.path.insert(0, os.path.join(root, "scripts"))
    from version import version_info  # noqa: E402  (Datei liegt neben diesem Skript)
    return version_info(root)["ui"]


def build(root):
    version = ui_version(root).encode()
    web_dir = os.path.join(root, "web")
    out_dir = os.path.join(root, "src", "generated")
    out_file = os.path.join(out_dir, "web_assets.h")

    files = []
    for dirpath, _, names in os.walk(web_dir):
        for name in sorted(names):
            full = os.path.join(dirpath, name)
            rel = "/" + os.path.relpath(full, web_dir).replace(os.sep, "/")
            ext = os.path.splitext(name)[1].lower()
            if ext not in MIME_TYPES:
                continue
            files.append((rel, full, MIME_TYPES[ext]))
    files.sort()

    lines = [
        "// AUTOMATISCH ERZEUGT von scripts/embed_web.py - nicht von Hand bearbeiten!",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "struct WebAsset {",
        "  const char* path;",
        "  const char* mime;",
        "  const uint8_t* data;",
        "  size_t len;",
        "};",
        "",
    ]
    total_raw = total_gz = 0
    for rel, full, _ in files:
        with open(full, "rb") as f:
            raw = f.read()
        raw = raw.replace(b"__HEXAPOD_UI_VERSION__", version)  # Kennung der Oberfläche
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        total_raw += len(raw)
        total_gz += len(gz)
        lines.append("static const uint8_t %s[] = {" % c_ident(rel))
        for i in range(0, len(gz), 20):
            lines.append("  " + ",".join(str(b) for b in gz[i:i + 20]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("static const WebAsset WEB_ASSETS[] = {")
    for rel, _, mime in files:
        ident = c_ident(rel)
        lines.append('  {"%s", "%s", %s, sizeof(%s)},' % (rel, mime, ident, ident))
    lines.append("};")
    lines.append("static const size_t WEB_ASSETS_COUNT = %d;" % len(files))
    content = "\n".join(lines) + "\n"

    os.makedirs(out_dir, exist_ok=True)
    old = None
    if os.path.exists(out_file):
        with open(out_file, "r", encoding="utf-8") as f:
            old = f.read()
    if old != content:  # nur bei Änderung schreiben -> kein unnötiges Neukompilieren
        with open(out_file, "w", encoding="utf-8") as f:
            f.write(content)
    print("embed_web: %d Dateien, %d -> %d Bytes (gzip)" % (len(files), total_raw, total_gz))


build(project_dir())
