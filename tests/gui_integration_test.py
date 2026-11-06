# Integration smoke test for the P2 worker-isolation wiring.
# Launches the GUI with a media file as a CLI argument (which auto-triggers
# Analysis.start -> spawns analyzer-worker.exe), and verifies:
#   1. the GUI starts and survives (does not crash),
#   2. analyzer-worker.exe is actually spawned,
#   3. the worker exits once it has emitted its result (no leaked process),
#   4. no minidump is produced for a well-behaved input.
#
# Usage:
#   python tests/gui_integration_test.py                  # build tree
#   python tests/gui_integration_test.py --app dist/MediaAnalyse-v1.0/media-analyse.exe
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The Qt targets (app / controller test) are laid out flat in the app preset's
# build root; windeployqt deploys the Qt DLLs + plugins next to the exe there.
APP = os.path.join(ROOT, "build", "win-mingw-app", "media-analyse.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")


def running(name):
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"],
                             capture_output=True, timeout=10).stdout  # bytes
        return name.lower().encode() in out.lower()
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description="GUI + worker isolation smoke test")
    ap.add_argument("--app", default=APP, help="path to media-analyse.exe")
    ap.add_argument("--sample", default=SAMPLE, help="media file to analyse")
    args = ap.parse_args()
    app = os.path.abspath(args.app)
    app_dir = os.path.dirname(app)
    sample = os.path.abspath(args.sample)

    if not os.path.exists(app):
        print(f"FAIL: app not built at {app}")
        return 1
    if not os.path.exists(sample):
        print(f"FAIL: sample missing at {sample}")
        return 1

    env = os.environ.copy()
    # Use the real platform (the PRISM UI renders correctly there; offscreen
    # crashes when a populated frame table is drawn, which is unrelated to the
    # worker wiring under test).

    log_path = os.path.join(app_dir, "gui_integration.log")
    with open(log_path, "wb") as log:
        p = subprocess.Popen([app, sample], env=env, stdout=log, stderr=subprocess.STDOUT)
        print(f"launched GUI pid={p.pid}")

        # Wait for the GUI to come up and spawn the worker. Poll tightly: the
        # worker finishes in ~50 ms on a short clip, so a coarse 200 ms poll can
        # miss it entirely.
        worker_seen = False
        for _ in range(800):  # up to ~16s at 20 ms
            if running("analyzer-worker.exe"):
                worker_seen = True
                print("OK: analyzer-worker.exe spawned by GUI")
                break
            if p.poll() is not None:
                print(f"FAIL: GUI exited early with code {p.returncode}")
                return 1
            time.sleep(0.02)

        if not worker_seen:
            print("FAIL: analyzer-worker.exe was never spawned")
            p.terminate()
            return 1

        # The worker must exit once it has emitted its result line. A worker that
        # lingers means the process is leaked for every analysis (it used to block
        # joining its stdin reader thread), so treat it as a failure.
        worker_gone = False
        for _ in range(50):  # up to ~10s
            if not running("analyzer-worker.exe"):
                worker_gone = True
                break
            time.sleep(0.2)
        if not worker_gone:
            print("FAIL: analyzer-worker.exe still running 10s after the result "
                  "(leaked worker process)")
            p.terminate()
            return 1
        print("OK: worker exited after emitting its result")

        # GUI should still be alive after the worker finished.
        if p.poll() is None:
            print("OK: GUI still alive after worker completion")
        else:
            print(f"FAIL: GUI died (exit {p.returncode}) after worker finished")
            return 1

        # No crash dump expected for a good file.
        crashes = os.path.join(app_dir, "crashes")
        dumps = []
        if os.path.isdir(crashes):
            dumps = [f for f in os.listdir(crashes) if f.endswith(".dmp")]
        if dumps:
            print(f"FAIL: unexpected crash dumps: {dumps}")
            return 1
        print("OK: no crash dump produced")

        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
    print("INTEGRATION SMOKE: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
