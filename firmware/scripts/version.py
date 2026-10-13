"""Versionsangaben für Firmware und Oberfläche.

Version aus der Datei VERSION (von Hand pflegen, siehe CHANGELOG.md), dazu der
Git-Stand (kurzer Commit-Hash, "+" bei nicht eingecheckten Änderungen) und das
Build-Datum. Als PlatformIO-Pre-Script erzeugt es src/generated/version.h;
embed_web.py setzt dieselbe Kennung in die Oberfläche ein.
"""

import datetime
import os
import subprocess


def project_dir():
    try:
        Import("env")  # noqa: F821  (nur unter PlatformIO definiert)
        return env.subst("$PROJECT_DIR")  # noqa: F821
    except NameError:
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _git(root, *args):
    try:
        out = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, timeout=5)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def version_info(root):
    with open(os.path.join(root, "VERSION"), encoding="utf-8") as f:
        version = f.read().strip()
    commit = _git(root, "rev-parse", "--short=7", "HEAD")
    if commit and _git(root, "status", "--porcelain", "--untracked-files=no", "--", "."):
        commit += "+"  # mit lokalen Änderungen gebaut
    built = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    # Kennung der Oberfläche: Version und Commit (ohne Datum, ändert sich nur mit dem Stand)
    ui = version + ("-" + commit if commit else "")
    return {"version": version, "git": commit, "built": built, "ui": ui}


def write_header(root):
    info = version_info(root)
    out_dir = os.path.join(root, "src", "generated")
    out_file = os.path.join(out_dir, "version.h")
    content = "\n".join([
        "// AUTOMATISCH ERZEUGT von scripts/version.py - nicht von Hand bearbeiten!",
        "#pragma once",
        '#define HEXAPOD_FW_VERSION "%s"' % info["version"],
        '#define HEXAPOD_FW_GIT "%s"' % info["git"],
        '#define HEXAPOD_FW_BUILT "%s"' % info["built"],
        '#define HEXAPOD_UI_VERSION "%s"' % info["ui"],
        "",
    ])
    os.makedirs(out_dir, exist_ok=True)
    old = None
    if os.path.exists(out_file):
        with open(out_file, encoding="utf-8") as f:
            old = f.read()
    if old != content:
        with open(out_file, "w", encoding="utf-8") as f:
            f.write(content)
    print("version: %s (%s, %s)" % (info["version"], info["git"] or "ohne Git", info["built"]))


if __name__ != "version":  # nicht beim Import aus embed_web.py
    write_header(project_dir())
