#!/usr/bin/env python3
"""One-click, account-free toolchain deployment for MediaAnalyse.

The download/install root is NOT hardcoded. It is resolved in this order:

  1. --root <path>            (explicit override)
  2. MA_TOOLCHAIN_ROOT        (environment override)
  3. scan directories next to the MediaAnalyse project for an existing
     toolchain root (mingw64/, Qt/*/mingw_64/, ffmpeg-*/include/)
  4. <parent of MediaAnalyse>/CodeTools   (created when nothing is found)

After deployment the script writes CMakeUserPresets.json with two local
presets (local-win-mingw / local-win-mingw-app) that point at the resolved
root, so the tracked CMakePresets.json stays untouched.

Usage
-----
    python scripts/setup_toolchain.py --scan-only
    python scripts/setup_toolchain.py
    python scripts/setup_toolchain.py --skip-qt        # Qt installed by hand
    python scripts/setup_toolchain.py --root E:/Tools  # explicit location
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SIBLINGS_ROOT = PROJECT_ROOT.parent

FFMPEG_DIR = "ffmpeg-n9.0-latest-win64-gpl-shared-9.0"
FFMPEG_URL = (
    "https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip"
)
FFMPEG_SHA_URL = (
    "https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/download/latest/"
    "checksums.sha256"
)

MINGW_ZIP = "winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip"
MINGW_BASE = (
    "https://github.com/brechtsanders/winlibs_mingw/releases/download/"
    "16.2.0posix-23.1.3-14.0.0-ucrt-r3/"
)
MINGW_URL = MINGW_BASE + MINGW_ZIP
MINGW_SHA_URL = MINGW_URL + ".sha256"

NINJA_ZIP = "ninja-win.zip"
NINJA_URL = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/" + NINJA_ZIP
# Verified against the v1.13.2 release asset digest on GitHub.
NINJA_SHA256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"

QT_VERSION = "6.10.3"
QT_ARCH = "win64_mingw"                 # -> Qt/6.10.3/mingw_64
QT_MINGW_VARIANT = "qt.tools.win64_mingw1510"  # -> Qt/Tools/mingw1510_64


def log(msg: str) -> None:
    print(f"[setup] {msg}", flush=True)


def fail(msg: str) -> None:
    raise SystemExit(f"[setup] ERROR: {msg}")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_text(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "MediaAnalyse-setup/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode("utf-8", errors="replace")


def expected_sha256_from_text(text: str, filename: str) -> str | None:
    for line in text.splitlines():
        parts = line.replace("*", " ").split()
        if len(parts) >= 2 and parts[-1] == filename:
            digest = parts[0].lower()
            if len(digest) == 64 and all(c in "0123456789abcdef" for c in digest):
                return digest
    return None


def download(url: str, dest: Path, sha256: str, label: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and sha256_file(dest) == sha256:
        log(f"{label}: already present, checksum OK ({dest.name})")
        return dest

    tmp = dest.with_name(dest.name + ".part")
    if tmp.exists():
        tmp.unlink()
    log(f"{label}: downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "MediaAnalyse-setup/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r, tmp.open("wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            last_pct = -1
            while True:
                chunk = r.read(1024 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if total:
                    pct = done * 100 // total
                    if pct != last_pct and pct % 10 == 0:
                        log(f"{label}: {pct}% ({done // 1048576}/{total // 1048576} MB)")
                        last_pct = pct
    except Exception as exc:  # pragma: no cover - network fallback
        log(f"{label}: urllib failed ({exc}); retrying with curl")
        curl = shutil.which("curl")
        if not curl:
            fail(f"cannot download {label}: neither urllib nor curl works")
        subprocess.run([curl, "-fL", "-o", str(tmp), url], check=True)

    got = sha256_file(tmp)
    if got != sha256:
        tmp.unlink(missing_ok=True)
        fail(f"{label}: SHA-256 mismatch: expected {sha256}, got {got}")
    tmp.replace(dest)
    log(f"{label}: checksum OK")
    return dest


def safe_extract(zip_path: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        for name in z.namelist():
            p = Path(name)
            if p.is_absolute() or ".." in p.parts:
                fail(f"unsafe path inside {zip_path.name}: {name}")
        z.extractall(dest)
    log(f"extracted {zip_path.name} -> {dest}")


def looks_like_root(p: Path) -> tuple[int, list[str]]:
    score = 0
    reasons: list[str] = []
    if (p / "mingw64" / "bin" / "gcc.exe").exists():
        score += 1
        reasons.append("mingw64/bin/gcc.exe")
    if any((p / "Qt").glob("6.*/mingw_64/bin/qmake.exe")):
        score += 1
        reasons.append("Qt/*/mingw_64/bin/qmake.exe")
    if any(p.glob("ffmpeg-*/include/libavformat/avformat.h")):
        score += 1
        reasons.append("ffmpeg-*/include/libavformat/avformat.h")
    return score, reasons


def scan_siblings() -> list[tuple[int, Path, list[str]]]:
    found: list[tuple[int, Path, list[str]]] = []
    if not SIBLINGS_ROOT.is_dir():
        return found
    for p in sorted(SIBLINGS_ROOT.iterdir()):
        if not p.is_dir() or p.resolve() == PROJECT_ROOT.resolve():
            continue
        score, reasons = looks_like_root(p)
        if score:
            found.append((score, p, reasons))
    found.sort(key=lambda item: (-item[0], str(item[1])))
    return found


def choose_root(args: argparse.Namespace) -> Path:
    if args.root:
        root = Path(args.root).expanduser().resolve()
        log(f"使用 --root 指定的目录: {root}")
        return root
    env = os.environ.get("MA_TOOLCHAIN_ROOT")
    if env:
        root = Path(env).expanduser().resolve()
        log(f"使用 MA_TOOLCHAIN_ROOT: {root}")
        return root

    found = scan_siblings()
    if found:
        score, root, reasons = found[0]
        log(f"在同级目录发现工具链: {root} (匹配 {score}/3: {', '.join(reasons)})")
        for _, other, _ in found[1:]:
            log(f"  其他候选: {other}")
        return root

    root = (SIBLINGS_ROOT / "CodeTools").resolve()
    log(f"同级目录未发现工具链，将创建: {root}")
    return root


def ensure_ffmpeg(root: Path) -> Path:
    target = root / FFMPEG_DIR
    if (target / "include" / "libavformat" / "avformat.h").exists():
        log(f"FFmpeg 开发包已就位: {target}")
        return target

    sha_text = fetch_text(FFMPEG_SHA_URL)
    sha = expected_sha256_from_text(sha_text, FFMPEG_DIR + ".zip")
    if not sha:
        fail("FFmpeg checksums.sha256 中未找到对应条目")
    z = download(FFMPEG_URL, root / (FFMPEG_DIR + ".zip"), sha, "FFmpeg 9.0 shared")
    safe_extract(z, root)
    if not target.exists():
        for p in root.iterdir():
            if p.is_dir() and (p / "include" / "libavformat" / "avformat.h").exists():
                p.rename(target)
                break
    if not (target / "include" / "libavformat" / "avformat.h").exists():
        fail(f"FFmpeg 解压后未找到头文件: {target}")
    return target


def ensure_mingw_and_ninja(root: Path) -> tuple[Path, Path]:
    mingw = root / "mingw64"
    gcc = mingw / "bin" / "gcc.exe"
    ninja = mingw / "bin" / "ninja.exe"

    if not gcc.exists():
        sha = expected_sha256_from_text(fetch_text(MINGW_SHA_URL), MINGW_ZIP)
        if not sha:
            fail("WinLibs .sha256 中未找到对应条目")
        z = download(MINGW_URL, root / MINGW_ZIP, sha, "GCC 16.2.0 (WinLibs)")
        safe_extract(z, root)
        if not gcc.exists():
            for p in root.iterdir():
                if p.is_dir() and (p / "bin" / "gcc.exe").exists():
                    if mingw.exists():
                        fail(f"{mingw} 已存在，无法自动改名 {p}")
                    p.rename(mingw)
                    break
    if not gcc.exists():
        fail(f"未找到 {gcc}")
    log(f"GCC 已就位: {gcc}")

    if not ninja.exists():
        z = download(NINJA_URL, root / NINJA_ZIP, NINJA_SHA256, "Ninja 1.13.2")
        safe_extract(z, mingw / "bin")
    if not ninja.exists():
        fail(f"未找到 {ninja}")
    log(f"Ninja 已就位: {ninja}")
    return gcc, ninja


def aqt_python(root: Path) -> Path:
    venv = root / ".aqt-venv"
    py = venv / "Scripts" / "python.exe" if os.name == "nt" else venv / "bin" / "python"
    if not py.exists():
        log(f"创建隔离的 aqtinstall 环境: {venv}")
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    probe = subprocess.run(
        [str(py), "-c", "import aqt"], capture_output=True, text=True
    )
    if probe.returncode != 0:
        log("安装 aqtinstall 到隔离环境")
        subprocess.run(
            [str(py), "-m", "pip", "install", "-U", "pip", "aqtinstall"], check=True
        )
    return py


def ensure_qt(root: Path, py: Path) -> tuple[Path, Path]:
    qt_root = root / "Qt"
    qmake = qt_root / QT_VERSION / "mingw_64" / "bin" / "qmake.exe"
    qt_gcc = qt_root / "Tools" / "mingw1510_64" / "bin" / "gcc.exe"

    if not qmake.exists():
        log(f"用 aqtinstall 安装 Qt {QT_VERSION} (arch={QT_ARCH})，无需账号")
        subprocess.run(
            [str(py), "-m", "aqt", "install-qt", "-O", str(qt_root),
             "windows", "desktop", QT_VERSION, QT_ARCH],
            check=True,
        )
    if not qt_gcc.exists():
        log("安装 Qt 配套 MinGW 15.1.0 工具链")
        subprocess.run(
            [str(py), "-m", "aqt", "install-tool", "-O", str(qt_root),
             "windows", "desktop", "tools_mingw1510", QT_MINGW_VARIANT],
            check=True,
        )
    if not qmake.exists() or not qt_gcc.exists():
        fail(f"Qt 安装不完整: qmake={qmake.exists()}, gcc={qt_gcc.exists()}")
    log(f"Qt 已就位: {qmake}")
    log(f"Qt MinGW 15.1 已就位: {qt_gcc}")
    return qmake, qt_gcc


def ensure_qt_backend_dll(root: Path) -> None:
    """Make MinGW's libexec backends self-contained.

    cc1plus.exe / collect2.exe live under libexec/ and load
    libwinpthread-1.dll from PATH. PowerShell/CMake do not have the toolchain
    bin directory on PATH, so the compiler test fails silently. Copying the
    DLL next to those backends fixes it for every launcher.
    """
    bin_dir = root / "Qt" / "Tools" / "mingw1510_64" / "bin"
    src = bin_dir / "libwinpthread-1.dll"
    if not src.exists():
        return
    gcc_root = bin_dir.parent / "libexec" / "gcc"
    if not gcc_root.is_dir():
        return
    for variant in gcc_root.glob("x86_64-w64-mingw32/*"):
        if not (variant / "cc1plus.exe").exists():
            continue
        dst = variant / "libwinpthread-1.dll"
        if not dst.exists():
            shutil.copy2(src, dst)
            log(f"补齐编译器后端依赖: {dst}")


def slash(path: Path) -> str:
    return str(path).replace("\\", "/")


def write_user_presets(root: Path) -> Path:
    ffmpeg = root / FFMPEG_DIR
    mingw_bin = root / "mingw64" / "bin"
    qt_prefix = root / "Qt" / QT_VERSION / "mingw_64"
    qt_bin = qt_prefix / "bin"
    qt_gcc_bin = root / "Qt" / "Tools" / "mingw1510_64" / "bin"

    common_ffmpeg = {
        "MA_FFMPEG_ROOT": slash(ffmpeg),
        "MA_FFMPEG_RUNTIME_DLL_DIR": slash(ffmpeg / "bin"),
    }
    ninja_var = {"CMAKE_MAKE_PROGRAM": slash(mingw_bin / "ninja.exe")}
    # g++ from MinGW-Builds locates as.exe/ld.exe via PATH. CMake 3.26 rejects
    # `$env{PATH}` when the map key is exactly "PATH" (self-reference), so use
    # "Path" — Windows env vars are case-insensitive, CMake's map is not.
    cli_env = {"Path": slash(mingw_bin) + ";$env{PATH}"}
    app_env = {
        "Path": ";".join(
            [slash(qt_gcc_bin), slash(qt_bin), slash(mingw_bin), "$env{PATH}"]
        )
    }

    data = {
        "version": 6,
        "configurePresets": [
            {
                "name": "local-win-mingw",
                "displayName": "Local MinGW (CLI only, GCC 16)",
                "inherits": "win-mingw",
                "environment": cli_env,
                "cacheVariables": {
                    "CMAKE_C_COMPILER": slash(mingw_bin / "gcc.exe"),
                    "CMAKE_CXX_COMPILER": slash(mingw_bin / "g++.exe"),
                    **ninja_var,
                    **common_ffmpeg,
                },
            },
            {
                "name": "local-win-mingw-app",
                "displayName": "Local MinGW + Qt Quick app (GCC 15.1)",
                "inherits": "win-mingw-app",
                "environment": app_env,
                "cacheVariables": {
                    "CMAKE_C_COMPILER": slash(qt_gcc_bin / "gcc.exe"),
                    "CMAKE_CXX_COMPILER": slash(qt_gcc_bin / "g++.exe"),
                    **ninja_var,
                    "MA_ENABLE_APP": "ON",
                    "CMAKE_PREFIX_PATH": slash(qt_prefix),
                    "MA_QT_RUNTIME_DIR": slash(qt_bin),
                    **common_ffmpeg,
                },
            },
        ],
        "buildPresets": [
            {
                "name": "local-win-mingw",
                "configurePreset": "local-win-mingw",
                "environment": cli_env,
            },
            {
                "name": "local-win-mingw-app",
                "configurePreset": "local-win-mingw-app",
                "environment": app_env,
            },
        ],
    }
    out = PROJECT_ROOT / "CMakeUserPresets.json"
    out.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    log(f"已生成 {out}（工具链根目录: {root}）")
    return out


def ensure_gitignore() -> None:
    gi = PROJECT_ROOT / ".gitignore"
    text = gi.read_text(encoding="utf-8") if gi.exists() else ""
    if "CMakeUserPresets.json" not in text:
        addition = (
            "\n# --- 本地工具链路径（由 scripts/setup_toolchain.py 生成） ---\n"
            "CMakeUserPresets.json\n"
        )
        gi.write_text(text.rstrip("\n") + "\n" + addition, encoding="utf-8")
        log("已把 CMakeUserPresets.json 加入 .gitignore")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="explicit toolchain root (overrides scanning)")
    ap.add_argument("--scan-only", action="store_true", help="only list sibling candidates")
    ap.add_argument("--skip-qt", action="store_true", help="skip aqtinstall (Qt installed manually)")
    args = ap.parse_args()

    log(f"项目目录: {PROJECT_ROOT}")
    found = scan_siblings()
    if found:
        log("同级目录扫描结果:")
        for score, p, reasons in found:
            log(f"  [{score}/3] {p}  <- {', '.join(reasons)}")
    else:
        log("同级目录扫描结果: 未发现现成工具链")

    if args.scan_only:
        return 0

    root = choose_root(args)
    root.mkdir(parents=True, exist_ok=True)

    ensure_ffmpeg(root)
    ensure_mingw_and_ninja(root)
    if args.skip_qt:
        log("按 --skip-qt 跳过 Qt 安装")
    else:
        ensure_qt(root, aqt_python(root))
    ensure_qt_backend_dll(root)
    write_user_presets(root)
    ensure_gitignore()

    log("环境部署完成。请在项目根目录执行（预设已内置所需 PATH）：")
    log(f'  cd "{PROJECT_ROOT}"')
    log("  cmake --preset local-win-mingw-app")
    log("  cmake --build --preset local-win-mingw-app --parallel 8")
    log("  python scripts/package.py --zip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
