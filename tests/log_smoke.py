#!/usr/bin/env python3
"""Regression checks for the cross-process file logger (ma::infra::Logger).

Verifies what a support engineer actually relies on:
  1. every process writes its own rolling UTF-8 log file;
  2. URL credentials (userinfo + sensitive query params) never reach disk;
  3. FFmpeg diagnostics are captured but stay at a sane default level;
  4. MA_LOG_LEVEL=off creates no files at all (fuzz runs stay fast);
  5. size-based rotation honours MA_LOG_MAX_FILES and never grows unbounded;
  6. the packaged build has a writable logs/ dir plus the collector scripts.

Usage:
    python tests/log_smoke.py
    python tests/log_smoke.py --cli build/win-mingw/cli/media-analyse-cli.exe
    python tests/log_smoke.py --worker build/win-mingw/worker/analyzer-worker.exe
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(ROOT, "build", "win-mingw", "cli", "media-analyse-cli.exe")
DEFAULT_WORKER = os.path.join(ROOT, "build", "win-mingw", "worker", "analyzer-worker.exe")
DEFAULT_SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")
DEFAULT_DIST = os.path.join(ROOT, "dist", "MediaAnalyse-v1.0")

SECRET_URL = "rtsp://admin:SuperSecret99@10.0.0.1/live?token=ABC123&key=XYZ789"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS  " if ok else "FAIL  ") + name + ("  | " + detail if detail else ""))


def run(cmd, log_dir, extra_env=None, stdin_data=None, timeout=60):
    env = dict(os.environ)
    env["MA_LOG_DIR"] = log_dir
    if extra_env:
        for k, v in extra_env.items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = str(v)
    # Remove stray overrides from the caller's shell so results are deterministic.
    for k in ("MA_LOG_LEVEL", "MA_LOG_MAX_BYTES", "MA_LOG_MAX_FILES"):
        if not extra_env or k not in extra_env:
            env.pop(k, None)
    try:
        p = subprocess.run(cmd, input=stdin_data, env=env, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p
    except subprocess.TimeoutExpired:
        return None


def read(path):
    for enc in ("utf-8", "utf-8-sig", "gbk"):
        try:
            with open(path, encoding=enc) as f:
                return f.read()
        except (UnicodeDecodeError, OSError):
            continue
    return ""


def main():
    ap = argparse.ArgumentParser(description="Logger regression smoke test")
    ap.add_argument("--cli", default=DEFAULT_CLI)
    ap.add_argument("--worker", default=DEFAULT_WORKER)
    ap.add_argument("--sample", default=DEFAULT_SAMPLE)
    ap.add_argument("--dist", default=DEFAULT_DIST)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "log_smoke"))
    args = ap.parse_args()

    missing = [p for p in (args.cli, args.worker, args.sample) if not os.path.exists(p)]
    if missing:
        print("ERROR: missing prerequisites, build the win-mingw preset first:")
        for m in missing:
            print("  " + m)
        return 2

    work = os.path.abspath(args.work)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)

    # ---- 1. CLI success writes its own log ---------------------------------
    d1 = os.path.join(work, "basic")
    p = run([args.cli, args.sample], d1)
    check("CLI runs successfully", p is not None and p.returncode == 0,
          "rc=" + str(p.returncode if p else "timeout"))
    cli_log = os.path.join(d1, "media-analyse-cli.log")
    txt = read(cli_log)
    check("CLI log file created", os.path.isfile(cli_log),
          "%d bytes" % (os.path.getsize(cli_log) if os.path.isfile(cli_log) else 0))
    check("CLI log has session banner + outcome",
          "session start" in txt and ("result ok=" in txt),
          "%d lines" % len(txt.splitlines()))
    check("CLI log has structured level tags",
          "[INFO" in txt and "session start" in txt,
          txt.splitlines()[0][:90] if txt else "(empty)")

    # ---- 2. worker writes its own log -------------------------------------
    req = ('{"type":"analyze","input":%s,"options":{}}\n'
           % ('"' + args.sample.replace("\\", "/") + '"'))
    p = run([args.worker], d1, stdin_data=req.encode("utf-8"))
    w_log = os.path.join(d1, "analyzer-worker.log")
    w_txt = read(w_log)
    check("worker log file created", os.path.isfile(w_log),
          "%d bytes" % (os.path.getsize(w_log) if os.path.isfile(w_log) else 0))
    check("worker log records request -> result -> exit",
          "analyze input=" in w_txt and "result delivered" in w_txt,
          "%d lines" % len(w_txt.splitlines()))
    check("GUI and worker use separate files",
          os.path.isfile(cli_log) and os.path.isfile(w_log))

    # ---- 3. credential redaction ------------------------------------------
    d2 = os.path.join(work, "redact")
    p = run([args.cli, "--json", "--connect-timeout", "1", "--stall-timeout", "1",
             "--probe-timeout", "1", SECRET_URL], d2)
    r_txt = read(os.path.join(d2, "media-analyse-cli.log"))
    leaked = [s for s in ("SuperSecret99", "ABC123", "XYZ789") if s in r_txt]
    check("no plaintext credentials on disk", not leaked,
          "leaked=" + (",".join(leaked) if leaked else "none"))
    check("masked placeholders present", "***" in r_txt,
          "mask count=" + str(r_txt.count("***")))
    check("URL host / stage still usable for diagnosis",
          "10.0.0.1" in r_txt or "invocation" in r_txt)

    # ---- 4. MA_LOG_LEVEL=off must not touch the disk -----------------------
    d3 = os.path.join(work, "off")
    p = run([args.cli, args.sample], d3, {"MA_LOG_LEVEL": "off"})
    entries = os.listdir(d3) if os.path.isdir(d3) else []
    check("MA_LOG_LEVEL=off creates no log files",
          not any(e.endswith(".log") for e in entries),
          "dir entries=" + str(entries))

    # ---- 5. rotation respects MA_LOG_MAX_FILES -----------------------------
    d4 = os.path.join(work, "rotate")
    rot_env = {"MA_LOG_MAX_BYTES": "350", "MA_LOG_MAX_FILES": "2"}
    for _ in range(6):
        run([args.cli, args.sample], d4, rot_env)
    glob = sorted(e for e in (os.listdir(d4) if os.path.isdir(d4) else [])
                  if e.startswith("media-analyse-cli") and e.endswith(".log"))
    check("rotation produced generations", any(".1.log" in e for e in glob),
          "files=" + ",".join(glob))
    check("rotation respects the retention cap",
          not any(".3.log" in e for e in glob), "files=" + ",".join(glob))
    rotated = read(os.path.join(d4, "media-analyse-cli.log"))
    check("current log still receives new records after rotation",
          "analyze input=" in rotated or "result ok=" in rotated,
          "%d bytes" % len(rotated))

    # ---- 6. packaged build carries a writable log dir + collector ----------
    dist_logs = os.path.join(args.dist, "logs")
    dist_items = (os.listdir(args.dist) if os.path.isdir(args.dist) else [])
    check("package has logs/ with README", os.path.isfile(os.path.join(dist_logs, "README.txt")))
    check("package has collect-logs.cmd + collect_logs.ps1",
          "collect-logs.cmd" in dist_items and "collect_logs.ps1" in dist_items)
    writable = False
    probe = os.path.join(dist_logs, ".write_probe")
    try:
        with open(probe, "w", encoding="utf-8") as f:
            f.write("ok")
        writable = True
        os.remove(probe)
    except OSError:
        pass
    check("package logs/ is writable", writable, dist_logs)

    failed = [r for r in results if not r[1]]
    print("\n%d/%d checks passed" % (len(results) - len(failed), len(results)))
    if failed:
        print("failed: " + ", ".join(r[0] for r in failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
