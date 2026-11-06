#!/usr/bin/env python3
"""End-to-end live-protocol check: MediaMTX + FFmpeg publishers + CLI + GUI stop."""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FF = "D:/CodeTools/ffmpeg-n9.0-latest-win64-gpl-shared-9.0/bin/ffmpeg.exe"
CLI = os.path.join(ROOT, "build", "win-mingw", "cli", "media-analyse-cli.exe")
GUI = os.path.join(ROOT, "build", "win-mingw-app", "media-analyse.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")
MTX = os.path.join(HERE, "mediamtx", "mediamtx.exe")
MTX_DIR = os.path.join(HERE, "mediamtx")

import ma_gui_check as G  # noqa: E402

procs = []
logs = {}


def spawn(name, cmd, cwd=None, env=None):
    log = open(os.path.join(HERE, "live_%s.log" % name), "w",
               encoding="utf-8", errors="replace")
    p = subprocess.Popen(cmd, cwd=cwd, stdout=log, stderr=log,
                         creationflags=0x00000008, env=env)
    logs[name] = log
    procs.append((name, p))
    print("START", name, "pid", p.pid)
    return p


def cli(args, timeout=40):
    r = subprocess.run([CLI] + args, capture_output=True, text=True,
                       errors="replace", timeout=timeout)
    try:
        return json.loads(r.stdout)
    except Exception as exc:
        return {"__parse_error__": str(exc), "stdout": r.stdout[:500],
                "stderr": r.stderr[:500]}


def main():
    # --- servers -------------------------------------------------------
    spawn("mediamtx", [MTX], cwd=MTX_DIR)
    time.sleep(2.0)
    spawn("rtsp_pub", [FF, "-hide_banner", "-loglevel", "warning",
                       "-re", "-stream_loop", "-1", "-i", SAMPLE,
                       "-c", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                       "rtsp://127.0.0.1:8554/live"])
    spawn("rtmp_pub", [FF, "-hide_banner", "-loglevel", "warning",
                       "-re", "-stream_loop", "-1", "-i", SAMPLE,
                       "-c", "copy", "-f", "flv",
                       "rtmp://127.0.0.1:1935/rtmp"])
    time.sleep(3.0)
    # Warm up the HLS muxer so the playlist has segments before we analyse it.
    try:
        subprocess.run([FF, "-hide_banner", "-loglevel", "error", "-i",
                        "http://127.0.0.1:8888/live/index.m3u8",
                        "-t", "1", "-f", "null", "-"],
                       capture_output=True, timeout=25)
    except Exception as exc:
        print("hls warmup:", exc)
    time.sleep(1.0)

    results = {}
    cases = [
        ("rtsp", ["--json", "--frames=8", "--max-frames", "120",
                  "--connect-timeout", "25", "--stall-timeout", "25",
                  "--opt", "rtsp_transport=tcp", "rtsp://127.0.0.1:8554/live"]),
        ("rtmp", ["--json", "--frames=8", "--max-frames", "120",
                  "--connect-timeout", "25", "--stall-timeout", "25",
                  "rtmp://127.0.0.1:1935/rtmp"]),
        ("hls", ["--json", "--frames=8", "--max-frames", "120",
                 "--connect-timeout", "25", "--stall-timeout", "25",
                 "http://127.0.0.1:8888/live/index.m3u8"]),
    ]
    for name, args in cases:
        try:
            d = cli(args)
            results[name] = {
                "ok": d.get("ok"),
                "code": (d.get("diagnostic") or {}).get("code") or d.get("error", {}).get("code"),
                "format": d.get("format", {}).get("name") if isinstance(d.get("format"), dict) else d.get("format"),
                "totalPackets": d.get("totalPackets"),
                "frames": len(d.get("frames") or []),
                "streams": len(d.get("streams") or []),
                "keys": sorted(d.keys())[:14],
            }
        except Exception as exc:
            results[name] = {"exception": str(exc)}
        print("CLI", name, results[name])

    # --- GUI: start live RTSP, then press Stop --------------------------
    env = dict(os.environ)
    env["QT_QUICK_BACKEND"] = "software"
    env["QSG_RHI_BACKEND"] = "software"
    gui_log = open(os.path.join(HERE, "live_gui_stderr.log"), "w",
                   encoding="utf-8", errors="replace")
    g = subprocess.Popen([GUI, "rtsp://127.0.0.1:8554/live"],
                         cwd=os.path.dirname(GUI), stdout=gui_log,
                         stderr=gui_log, creationflags=0x00000008, env=env)
    procs.append(("gui", g))
    time.sleep(4.0)
    wins = G.find_window()
    print("GUI windows", wins)
    if wins:
        hwnd = wins[0][0]
        G.user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
        time.sleep(0.3)
        l, t, r, b = G.rect(hwnd)
        cx0, cy0, cw, ch = G.client_origin(hwnd)
        G.shot(hwnd, os.path.join(HERE, "live_before_stop.png"), True)
        # Stop button in the LIVE status row
        G.post_click(hwnd, l, t, cx0, cy0, 1754, 370)
        time.sleep(1.5)
        G.shot(hwnd, os.path.join(HERE, "live_after_stop.png"), True)
        print("GUI SHOTS OK")
    gui_log.flush()

    print("RESULT_JSON=" + json.dumps(results, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    finally:
        for name, p in reversed(procs):
            try:
                p.terminate()
                p.wait(timeout=3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
            print("STOP", name)
        for lg in logs.values():
            lg.close()
