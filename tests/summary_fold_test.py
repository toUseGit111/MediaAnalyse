#!/usr/bin/env python3
"""Regression for the collapsible SUMMARY group (OVERVIEW + STREAMS).

Why: on a small window the 82 px metric strip and the 102-226 px stream cards
used to squeeze the frame table down to a handful of rows. Both now live in a
single foldable group; DATAVIEW (Layout.fillHeight) takes the freed space.

Two layers of checks:
  1. structural - the QML contract (fold bindings, always-visible NOTICE,
     fillHeight table, keyboard shortcut) must not regress;
  2. live       - launch the real GUI, press Ctrl+Shift+S, and compare window
     captures. Frame-row separators must move up and become more numerous,
     i.e. the user can actually see more packet rows after folding.

Needs the Qt runtime DLLs on PATH. Pure standard library + the existing
tests/manual/ma_gui_check.py Win32 capture helper.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import importlib.util
import os
import subprocess
import sys
import time
import zlib
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_APP = os.path.join(ROOT, "build", "win-mingw-app", "media-analyse.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")
MAIN_QML = os.path.join(ROOT, "app", "qml", "Main.qml")
HELPER = os.path.join(ROOT, "tests", "manual", "ma_gui_check.py")
ROW_LINE = (29, 34, 41)  # Theme.rowLine #1D2229


def load_helper():
    spec = importlib.util.spec_from_file_location("ma_gui_check", HELPER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if tag == b"IHDR":
            w, h = struct.unpack(">II", payload[:8])
        elif tag == b"IDAT":
            idat += payload
    return w, h, zlib.decompress(idat)


def find_summary_header_y(path):
    """Locate the 34 px (logical) rounded header band by its sunken fill.

    The first wide sunken band below SOURCE is the SUMMARY header; the metric
    strip comes later and is much taller. Working from the capture means the
    test does not hard-code a DPI-scaled click coordinate.
    """
    w, h, raw = read_png(path)
    stride = w * 3 + 1

    def header_fill(x, y):
        o = y * stride + 1 + x * 3
        c = tuple(raw[o:o + 3])
        # Theme.sunken normally, Theme.surface2 while the mouse hovers.
        return c in ((16, 19, 23), (35, 40, 48))

    for y0 in range(300, min(800, h - 60), 2):
        ys = range(y0, y0 + 48)
        if (sum(1 for y in ys if header_fill(900, y)) >= 33 and
                sum(1 for y in ys if header_fill(60, y)) >= 24 and
                sum(1 for y in ys if header_fill(1700, y)) >= 24):
            return y0 + 24
    return None


def frame_metrics(path):
    """Count frame-row separators and find the first one (capture pixels)."""
    w, h, raw = read_png(path)
    stride = w * 3 + 1

    def px(x, y):
        o = y * stride + 1 + x * 3
        return tuple(raw[o:o + 3])

    xs = (100, 300, 700, 1100, 1500)
    counts = [sum(1 for y in range(400, h - 60) if px(x, y) == ROW_LINE)
              for x in xs]
    first = next((y for y in range(400, h - 60)
                  if px(100, y) == ROW_LINE), None)
    blue = sum(1 for y in range(400, h - 60, 3)
               for x in (40, 60, 1800, 1820)
               if (lambda c: c[2] > 90 and c[2] > c[0] + 20)(px(x, y)))
    return {"size": (w, h), "separators": min(counts),
            "first": first, "blue": blue}


def window_pid(hwnd):
    pid = wt.DWORD()
    ctypes.windll.user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
    ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.environ.get("MA_APP", DEFAULT_APP))
    ap.add_argument("--wait", type=float, default=5.0)
    ap.add_argument("--skip-live", action="store_true")
    args = ap.parse_args()

    results = []

    def check(name, fn):
        try:
            detail = fn()
            print("PASS  %-34s %s" % (name, detail))
            return True
        except Exception as exc:
            print("FAIL  %-34s %s" % (name, exc))
            return False

    qml = open(MAIN_QML, encoding="utf-8").read()

    def fold_group():
        assert "id: summaryPanel" in qml
        assert qml.count("visible: summaryPanel.expanded") >= 2, \
            "OVERVIEW and STREAMS must both follow the fold state"
        return "one group, %d foldable sub-blocks" % qml.count(
            "visible: summaryPanel.expanded")
    results.append(check("single foldable SUMMARY group", fold_group))

    def notice_always_visible():
        assert qml.count("visible: root.noticeVisible") == 1
        seg = qml[qml.index("// 04 NOTICE"):qml.index("// STREAMS 内容")]
        assert "summaryPanel.expanded" not in seg, \
            "NOTICE must not be hidden when the summary is folded"
        return "NOTICE bound only to root.noticeVisible"
    results.append(check("NOTICE stays visible when folded", notice_always_visible))

    def table_fills_height():
        seg = qml[qml.index("07 DATAVIEW"):qml.index("08 STATUS")]
        assert "Layout.fillHeight: true" in seg
        return "DATAVIEW consumes the freed space"
    results.append(check("DATAVIEW fills freed height", table_fills_height))

    def keyboard_shortcut():
        assert "sequence: \"Ctrl+Shift+S\"" in qml
        assert "function toggleSummary()" in qml
        return "Ctrl+Shift+S + shared toggleSummary()"
    results.append(check("keyboard toggle available", keyboard_shortcut))

    if args.skip_live:
        print("SKIP  %-34s --skip-live" % "live fold comparison")
    else:
        def live():
            if not os.path.isfile(args.app):
                raise AssertionError("GUI not found: %s" % args.app)
            gui = load_helper()
            out_a = os.path.join(ROOT, "build", "summary_fold_before.png")
            out_b = os.path.join(ROOT, "build", "summary_fold_after.png")
            out_c = os.path.join(ROOT, "build", "summary_fold_restored.png")
            log_path = os.path.join(ROOT, "build", "summary_fold_gui.log")
            log_before = os.path.getsize(os.path.join(
                os.path.dirname(args.app), "logs", "media-analyse.log")) \
                if os.path.isfile(os.path.join(os.path.dirname(args.app),
                                               "logs", "media-analyse.log")) else 0
            logf = open(log_path, "w", encoding="utf-8", errors="replace")
            env = os.environ.copy()
            proc = subprocess.Popen(
                [args.app, SAMPLE.replace("\\", "/")],
                cwd=os.path.dirname(args.app), stdout=logf, stderr=logf,
                creationflags=0x00000008, env=env)
            try:
                time.sleep(args.wait)
                hwnd = None
                for cand, _ in gui.find_window():
                    if window_pid(cand) == proc.pid:
                        hwnd = cand
                        break
                assert hwnd, "GUI window for pid %d not found" % proc.pid
                gui.user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
                gui.user32.SetForegroundWindow(hwnd)
                time.sleep(0.4)
                l, t, _, _ = gui.rect(hwnd)
                before = gui.shot(hwnd, out_a)
                header_y = find_summary_header_y(out_a)
                assert header_y, "could not locate SUMMARY header in capture"
                # A real mouse click also activates the window, which the
                # keyboard shortcut needs on Windows.
                gui.click(l + 200, t + header_y)
                time.sleep(0.7)
                after = gui.shot(hwnd, out_b)
                gui.key("ctrl+shift+s")
                time.sleep(0.8)
                restored = gui.shot(hwnd, out_c)
                assert before == after == restored, "window resized during the test"
                a, b, c = (frame_metrics(p) for p in (out_a, out_b, out_c))
                assert a["separators"] > 0 and b["separators"] > 0, (a, b)
                assert b["separators"] >= a["separators"] * 1.8, \
                    "frame rows did not grow: %s -> %s" % (a["separators"], b["separators"])
                assert a["first"] and b["first"] and b["first"] < a["first"] - 150, \
                    "frame table did not move up: %s -> %s" % (a["first"], b["first"])
                assert b["blue"] > 0, "NOTICE / type chips disappeared"
                assert abs(c["separators"] - a["separators"]) <= max(2, a["separators"] * 0.2), \
                    "Ctrl+Shift+S did not restore the expanded layout: %s -> %s" % (
                        b["separators"], c["separators"])

                # The Qt message handler forwards console.log into the GUI log.
                gui_log = os.path.join(os.path.dirname(args.app), "logs",
                                       "media-analyse.log")
                logged = ""
                if os.path.isfile(gui_log):
                    with open(gui_log, "rb") as f:
                        f.seek(log_before)
                        logged = f.read().decode("utf-8", "replace")
                detail = ("header y=%d, rows %d->%d->%d, first y %d->%d, blue=%d" %
                          (header_y, a["separators"], b["separators"],
                           c["separators"], a["first"], b["first"], b["blue"]))
                if "[SUMMARY]" in logged:
                    detail += ", log=[SUMMARY] ok"
                return detail
            finally:
                try:
                    proc.terminate()
                except Exception:
                    pass
                logf.close()
        results.append(check("live fold shows more frame rows", live))

    ok = all(results)
    print("\n%d/%d checks passed" % (sum(results), len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
