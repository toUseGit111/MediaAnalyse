"""Regression for the live incremental publishing path.

Before this fix the worker emitted only one "result" line, after the demux loop
finished. A live stream has no EOF, so the GUI showed zero streams and zero
rows forever while packets kept arriving. The worker now emits:

  {"type":"streams", ...}   right after find_stream_info (container + streams)
  {"type":"frames", ...}    batches of newly parsed rows while demuxing
  {"type":"result", ...}    unchanged canonical complete result

This test drives analyzer-worker.exe over stdin/stdout on a normal local file
and asserts the snapshot contract: ordering, non-overlapping row batches, a
stable stream list and an unchanged final result.

Usage:
    python tests/live_snapshot_test.py
    python tests/live_snapshot_test.py --worker build/win-mingw/worker/analyzer-worker.exe
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKER = os.path.join(ROOT, "build", "win-mingw-app", "analyzer-worker.exe")
SAMPLE = os.path.join(ROOT, "tests", "assets", "sample.mp4")


def main():
    ap = argparse.ArgumentParser(description="worker live snapshot regression")
    ap.add_argument("--worker", default=WORKER, help="path to analyzer-worker.exe")
    ap.add_argument("--sample", default=SAMPLE, help="media file to analyse")
    args = ap.parse_args()

    worker = os.path.abspath(args.worker)
    sample = os.path.abspath(args.sample)
    for path, label in ((worker, "worker"), (sample, "sample")):
        if not os.path.isfile(path):
            print(f"FAIL: {label} missing at {path}")
            return 1

    env = os.environ.copy()
    env["MA_LOG_LEVEL"] = "off"  # keep the harness quiet; logs are not under test
    req = json.dumps({"type": "analyze", "input": sample,
                      "options": {"maxFrames": 200000}}) + "\n"
    try:
        p = subprocess.run([worker], input=req.encode("utf-8"),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=60, env=env)
    except subprocess.TimeoutExpired:
        print("FAIL: worker did not exit (leaked process / blocked on stdin)")
        return 1

    lines = []
    for raw in p.stdout.decode("utf-8", "replace").splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            lines.append(json.loads(raw))
        except json.JSONDecodeError:
            print(f"FAIL: non-JSON worker line: {raw[:160]}")
            return 1

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    types = [o.get("type") for o in lines]
    results = [o for o in lines if o.get("type") == "result"]
    streams = [o for o in lines if o.get("type") == "streams"]
    frames = [o for o in lines if o.get("type") == "frames"]
    progress = [o for o in lines if o.get("type") == "progress"]

    check("worker exits 0", p.returncode == 0, f"rc={p.returncode}")
    check("one final result line", len(results) == 1, f"count={len(results)}")
    check("final result ok", bool(results) and results[0].get("ok") is True)
    check("progress lines unchanged", len(progress) >= 1,
          f"count={len(progress)}")
    check("one streams snapshot", len(streams) == 1,
          f"count={len(streams)}")
    check("at least one frames batch", len(frames) >= 1,
          f"count={len(frames)}")

    if streams:
        s = streams[0]
        check("streams snapshot has stream list", len(s.get("streams", [])) > 0,
              f"count={len(s.get('streams', []))}")
        check("streams snapshot carries no frames", "frames" not in s)
        check("streams snapshot ok=true", s.get("ok") is True)
        check("streams snapshot has container format",
              bool(s.get("format", {}).get("name")))

    check("streams snapshot precedes frame batches",
          bool(streams and frames) and types.index("streams") <
          min(types.index("frames"), len(types)))
    check("all snapshots precede the final result",
          "result" in types and
          all(t in ("streams", "frames", "progress")
              for t in types[:types.index("result")]))

    seqs = [f.get("seq") for batch in frames for f in batch.get("frames", [])]
    check("frame batches are non-empty", bool(seqs))
    check("snapshot seq strictly increasing (no duplicate rows)",
          all(a < b for a, b in zip(seqs, seqs[1:])),
          f"rows={len(seqs)}")

    if results:
        final_rows = results[0].get("frames", [])
        final_seqs = [f.get("seq") for f in final_rows]
        check("final result keeps the complete row set",
              len(final_rows) >= len(seqs),
              f"snapshot={len(seqs)} final={len(final_rows)}")
        check("snapshot rows match final result prefix",
              final_seqs[:len(seqs)] == seqs)
        if streams:
            check("stream list matches final result",
                  streams[0].get("streams") == results[0].get("streams"))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        suffix = f" ({detail})" if detail and not ok else ""
        print(("OK: " if ok else "FAIL: ") + name + suffix)
    print(f"LIVE SNAPSHOT: {len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
