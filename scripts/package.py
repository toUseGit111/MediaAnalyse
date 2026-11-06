#!/usr/bin/env python3
"""Stage a clean, ready-to-run package from the app build tree.

The `win-mingw-app` build tree ends up flat (exe + Qt DLLs + windeployqt plugins
all in `build/win-mingw-app/`) because `qt_standard_project_setup()` redirects
runtime output there. That directory is therefore *almost* a green package -
but it also contains test executables, logs and crash dumps.

This script copies only what a package needs into `dist/`, so the result can be
zipped and handed to a colleague as-is.

Usage
-----
    python scripts/package.py                       # -> dist/MediaAnalyse-v1.0
    python scripts/package.py --with-cli            # also ship media-analyse-cli.exe
    python scripts/package.py --out dist/tmp --force

Layout produced
---------------
    dist/MediaAnalyse-v1.0/
        media-analyse.exe           GUI
        analyzer-worker.exe         crash-isolated analysis kernel
        *.dll                       Qt / FFmpeg / MinGW runtimes
        qt.conf                     Prefix=. (windeployqt writes Prefix=..)
        platforms/ imageformats/ qml/ ...      Qt plugins + QML modules
        README.md  PROJECT_GUIDE.md            usage docs
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build", "win-mingw-app")

# Directories windeployqt creates next to the exe; copy whichever exist.
PLUGIN_DIRS = [
    "platforms", "imageformats", "iconengines", "styles", "generic",
    "networkinformation", "tls", "qml", "qmltooling", "translations",
]

# Never ship these, even if they match a pattern above.
DENY_PREFIXES = ("ma_ser_test", "ma_controller_test")
DENY_SUFFIXES = (".log", ".dmp", ".pdb", ".ilk", ".exp", ".lib", ".a", ".obj")
DENY_DIRS = {"crashes", "CMakeFiles", ".qt", ".rcc", "media-analyse_autogen"}


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}"
        n /= 1024.0


def main():
    ap = argparse.ArgumentParser(description="Stage a clean MediaAnalyse package.")
    ap.add_argument("--build-dir", default=DEFAULT_BUILD)
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "MediaAnalyse-v1.0"))
    ap.add_argument("--with-cli", action="store_true",
                    help="also ship media-analyse-cli.exe (scripted analysis)")
    ap.add_argument("--force", action="store_true",
                    help="delete the output directory first if it exists")
    args = ap.parse_args()

    src = os.path.abspath(args.build_dir)
    dst = os.path.abspath(args.out)
    gui = os.path.join(src, "media-analyse.exe")
    if not os.path.exists(gui):
        print(f"ERROR: {gui} not found. Build the app preset first:\n"
              f"       cmake --preset win-mingw-app && "
              f"cmake --build --preset win-mingw-app")
        return 1

    if os.path.isdir(dst):
        if not args.force:
            print(f"ERROR: {dst} already exists (use --force to replace it)")
            return 1
        shutil.rmtree(dst)
    os.makedirs(dst)

    copied = 0

    def copy_file(name):
        nonlocal copied
        s = os.path.join(src, name)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(dst, name))
            copied += 1
            return True
        return False

    # Executables that make up the product.
    copy_file("media-analyse.exe")
    copy_file("analyzer-worker.exe")
    if not os.path.exists(os.path.join(dst, "analyzer-worker.exe")):
        print("ERROR: analyzer-worker.exe missing from the build tree")
        return 1
    if args.with_cli and not copy_file("media-analyse-cli.exe"):
        print("WARN: --with-cli requested but media-analyse-cli.exe not found")

    # Qt / FFmpeg / MinGW runtime DLLs.
    for name in sorted(os.listdir(src)):
        if name.lower().endswith(".dll"):
            copy_file(name)

    # qt.conf (Prefix=.) - windeployqt's default assumes a bin/ layout.
    copy_file("qt.conf")

    # Qt plugins + QML modules.
    for d in PLUGIN_DIRS:
        s = os.path.join(src, d)
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, d),
                            ignore=shutil.ignore_patterns("*.pdb", "*.log"))
            copied += 1

    # Documentation shipped with the tool.
    for doc, target in (("README.md", "README.md"),
                        ("docs/PROJECT_GUIDE.md", "PROJECT_GUIDE.md")):
        s = os.path.join(ROOT, doc)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(dst, target))
            copied += 1

    total = 0
    for base, dirs, files in os.walk(dst):
        dirs[:] = [d for d in dirs if d not in DENY_DIRS]
        for f in files:
            if f.startswith(DENY_PREFIXES) or f.endswith(DENY_SUFFIXES):
                continue
            total += os.path.getsize(os.path.join(base, f))

    print(f"package : {dst}")
    print(f"items   : {copied} top-level entries")
    print(f"size    : {human(total)}")
    print("run     : " + os.path.join(dst, "media-analyse.exe"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
