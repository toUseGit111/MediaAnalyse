"""End-to-end regression for a source that never reaches EOF.

The reported bug: an RTSP live stream kept reading packets ("解析中", packet
count growing) while the stream list and frame table stayed empty forever.
Root cause was that the GUI only published data in finishWith(), which is
never reached for a source without EOF.

This test starts a tiny local HTTP server that loops an MPEG-TS file forever
(chunked transfer, no Content-Length), launches the GUI with that URL and
asserts from the GUI log that:
  * the streams snapshot is consumed right after probing,
  * at least two distinct frame batches are consumed with a growing row count,
  * the worker is still running (no final "result"/"worker finished") while
    all of that is displayed.

No external media server is needed. ffmpeg CLI is used once to mux the sample
into MPEG-TS; if it is unavailable the test reports SKIP instead of failing.

Usage:
    python tests/live_stream_gui_test.py
    python tests/live_stream_gui_test.py --app dist/MediaAnalyse-v1.0/media-analyse.exe
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "build", "win-mingw-app", "media-analyse.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")
FFMPEG_GUESS = os.path.join(
    "D:", os.sep, "CodeTools", "ffmpeg-n9.0-latest-win64-gpl-shared-9.0",
    "bin", "ffmpeg.exe")

TS_CHUNK = 188 * 7          # whole MPEG-TS packets
CHUNK_DELAY = 0.01          # ~130 KB/s, keeps the stream alive for the test
ROW_RE = re.compile(r"live frames batch=\d+ rows=(\d+)")


def find_ffmpeg(explicit):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    if os.path.isfile(FFMPEG_GUESS):
        return FFMPEG_GUESS
    return shutil.which("ffmpeg")


def make_stream_server(ts_bytes, stop):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                while not stop.is_set():
                    for off in range(0, len(ts_bytes), TS_CHUNK):
                        if stop.is_set():
                            break
                        chunk = ts_bytes[off:off + TS_CHUNK]
                        self.wfile.write(
                            ("%X\r\n" % len(chunk)).encode("ascii") +
                            chunk + b"\r\n")
                        self.wfile.flush()
                        time.sleep(CHUNK_DELAY)
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def main():
    ap = argparse.ArgumentParser(description="live (no-EOF) GUI regression")
    ap.add_argument("--app", default=APP, help="path to media-analyse.exe")
    ap.add_argument("--sample", default=SAMPLE, help="media file to loop")
    ap.add_argument("--ffmpeg", default=None, help="path to ffmpeg.exe")
    ap.add_argument("--seconds", type=float, default=16.0,
                    help="observation window")
    args = ap.parse_args()

    app = os.path.abspath(args.app)
    sample = os.path.abspath(args.sample)
    for path, label in ((app, "app"), (sample, "sample")):
        if not os.path.isfile(path):
            print(f"FAIL: {label} missing at {path}")
            return 1
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg:
        print("SKIP: ffmpeg.exe not found; cannot mux the looped TS")
        return 0

    app_dir = os.path.dirname(app)
    gui_log = os.path.join(app_dir, "logs", "media-analyse.log")
    log_offset = os.path.getsize(gui_log) if os.path.isfile(gui_log) else 0

    def new_log_text():
        try:
            with open(gui_log, "rb") as f:
                f.seek(log_offset)
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""

    stop = threading.Event()
    with tempfile.TemporaryDirectory(prefix="ma-live-") as tmp:
        ts_path = os.path.join(tmp, "loop.ts")
        mux = subprocess.run(
            [ffmpeg, "-v", "error", "-i", sample, "-c", "copy",
             "-f", "mpegts", "-y", ts_path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        if mux.returncode != 0 or not os.path.isfile(ts_path):
            print("FAIL: ffmpeg could not mux the sample: " +
                  mux.stderr.decode("utf-8", "replace")[-300:])
            return 1
        with open(ts_path, "rb") as f:
            ts_bytes = f.read()
        if len(ts_bytes) < TS_CHUNK:
            print("FAIL: muxed TS is too small")
            return 1

        server = make_stream_server(ts_bytes, stop)
        url = "http://127.0.0.1:%d/live.ts" % server.server_address[1]
        env = os.environ.copy()
        # Batch lines are DEBUG (INFO is throttled to every 25th batch), so the
        # harness needs debug level to count every published batch.
        env["MA_LOG_LEVEL"] = "debug"
        env.pop("MA_LOG_DIR", None)
        print(f"streaming {len(ts_bytes)} TS bytes in a loop at {url}")

        with open(os.path.join(tmp, "gui.out"), "wb") as out:
            gui = subprocess.Popen([app, url], env=env, cwd=app_dir,
                                   stdout=out, stderr=subprocess.STDOUT)
            rows = []
            streams_seen = False
            worker_finished = False
            deadline = time.time() + args.seconds
            try:
                while time.time() < deadline:
                    if gui.poll() is not None:
                        print(f"FAIL: GUI exited early (rc={gui.returncode})")
                        return 1
                    txt = new_log_text()
                    if "live streams snapshot" in txt:
                        streams_seen = True
                    if "worker finished" in txt:
                        worker_finished = True
                    for n in ROW_RE.findall(txt):
                        if int(n) not in rows:
                            rows.append(int(n))
                    if streams_seen and len(rows) >= 3 and worker_finished is False:
                        break
                    time.sleep(0.2)
            finally:
                gui.terminate()
                try:
                    gui.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    gui.kill()
                stop.set()
                server.shutdown()
                server.server_close()

    checks = [
        ("streams snapshot consumed", streams_seen),
        ("frame batches keep arriving", len(rows) >= 3, f"batches={len(rows)}"),
        ("row count keeps growing", len(rows) >= 2 and rows[-1] > rows[0],
         f"rows={rows}"),
        ("worker still reading (no final result yet)", not worker_finished),
    ]
    failed = 0
    for item in checks:
        name, ok = item[0], item[1]
        detail = item[2] if len(item) > 2 else ""
        if not ok:
            failed += 1
        print(("OK: " if ok else "FAIL: ") + name +
              (f" ({detail})" if detail and not ok else ""))
    if failed:
        print("--- GUI log tail ---")
        print(new_log_text()[-1500:])
        return 1
    print("LIVE STREAM GUI: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
