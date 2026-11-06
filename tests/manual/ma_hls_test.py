#!/usr/bin/env python3
"""Focused HLS check with generous staged timeouts (validates probe phase)."""
import json
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FF = "D:/CodeTools/ffmpeg-n9.0-latest-win64-gpl-shared-9.0/bin/ffmpeg.exe"
CLI = os.path.join(ROOT, "build", "win-mingw", "cli", "media-analyse-cli.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")
MTX = os.path.join(HERE, "mediamtx", "mediamtx.exe")
MTX_DIR = os.path.join(HERE, "mediamtx")
procs = []


def spawn(cmd, cwd=None, log="hls_test_server.log"):
    f = open(os.path.join(HERE, log), "a", encoding="utf-8", errors="replace")
    p = subprocess.Popen(cmd, cwd=cwd, stdout=f, stderr=f,
                         creationflags=0x00000008)
    procs.append(p)
    return p


try:
    spawn([MTX], MTX_DIR)
    time.sleep(2)
    spawn([FF, "-hide_banner", "-loglevel", "warning", "-re",
           "-stream_loop", "-1", "-i", SAMPLE, "-c", "copy",
           "-f", "rtsp", "-rtsp_transport", "tcp",
           "rtsp://127.0.0.1:8554/hls"])
    time.sleep(4)
    # Warm up the HLS muxer so segments exist before the analyzer connects.
    subprocess.run([FF, "-hide_banner", "-loglevel", "error",
                    "-i", "http://127.0.0.1:8888/hls/index.m3u8",
                    "-t", "1", "-f", "null", "-"], timeout=25,
                   capture_output=True)
    time.sleep(1)
    for label, args in [
        ("default-probe", ["--json", "--frames=6", "--max-frames", "90",
                           "--connect-timeout", "25", "--stall-timeout", "25",
                           "http://127.0.0.1:8888/hls/index.m3u8"]),
        ("explicit-probe", ["--json", "--frames=6", "--max-frames", "90",
                            "--connect-timeout", "25", "--stall-timeout", "25",
                            "--probe-timeout", "60",
                            "http://127.0.0.1:8888/hls/index.m3u8"]),
    ]:
        t0 = time.time()
        try:
            r = subprocess.run([CLI] + args, capture_output=True, text=True,
                               errors="replace", timeout=70)
            try:
                d = json.loads(r.stdout)
            except Exception:
                print(label, "RAW rc=", r.returncode,
                      "stdout=", repr(r.stdout[:400]),
                      "stderr=", repr(r.stderr[:400]))
                raise
            print(label, "elapsed=%.1f" % (time.time() - t0),
                  "ok=", d.get("ok"),
                  "code=", (d.get("error") or {}).get("code"),
                  "format=", (d.get("format") or {}).get("name")
                  if isinstance(d.get("format"), dict) else d.get("format"),
                  "packets=", d.get("totalPackets"),
                  "streams=", len(d.get("streams") or []),
                  "notes=", str(d.get("notes"))[:120])
        except Exception as exc:
            print(label, "EXCEPTION", exc)
finally:
    for p in reversed(procs):
        try:
            p.terminate()
            p.wait(timeout=3)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass
