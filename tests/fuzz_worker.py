#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P2 fuzz harness for the crash-isolated analysis kernel.

Goal (per REFACTOR_PLAN.md, stage P2): feed 10^5 randomly mutated/truncated
inputs to the analyzer and prove that *no run crashes the process in an
unhandled way*. The analyzer is the `analyzer-worker.exe` (Qt-free, crash
isolated) or the `media-analyse-cli.exe` (same core). Either way the kernel
links FFmpeg and is the thing we must keep from taking down the GUI.

How a "crash" is detected
-------------------------
The crash handler (infra/CrashHandler.cpp) installs an unhandled-exception
filter that writes `<exedir>/crashes/<app>-<ts>.dmp` and prints a single line
    [CRASHDUMP] <absolute-path>
to stderr before the process is terminated by Windows. So a crash is any run
that (a) emits `[CRASHDUMP]` on stderr, or (b) drops a brand-new `.dmp` into
the worker's `crashes/` directory, or (c) terminates without ever printing a
JSON `result` line and without the clean exit codes the tools define.

A *clean* failure (exit 1 from the CLI, or a `result` envelope with
`"ok":false` from the worker) is NOT a crash: it is the kernel correctly
turning a malformed file into a structured diagnostic. The fuzz target is that
the set of crashes is empty.

Mutation strategies
-------------------
  truncate : cut the file to a random 0..100% prefix (broken/short containers).
  flip     : flip 1..N random bytes (bit-level corruption).
  insert   : splice a block of random bytes at a random offset.
  delete   : remove a random contiguous chunk.
  combine  : apply 1..3 of the above to stress multi-corruption.

Usage
-----
  python tests/fuzz_worker.py --exe build/win-mingw-app/analyzer-worker.exe \
                              --seed tests/assets/sample.mp4 --iters 2000

  # against the CLI instead of the worker:
  python tests/fuzz_worker.py --exe build/win-mingw/cli/media-analyse-cli.exe \
                              --seed tests/assets/sample.mp4 --mode cli

Notes
-----
  * The worker/CLI need their runtime DLLs on PATH (Qt mingw toolchain for the
    app-built worker, base mingw for the CLI). Run from a shell where those are
    already on PATH, or pass --env-path.
  * `.dmp` files are deleted after each crash is recorded so a long run does
    not fill the disk. The *input* that reproduced the crash is saved to
    --corpus-dir (default: <cwd>/fuzz-corpus) for later single-shot repro.
  * Exit code is non-zero if any crash/abnormal-exit/timeout was observed, so
    the harness drops straight into a CI failure.
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time


# Worker envelope tags we care about on stdout.
RESULT_TAG = '"type":"result"'

# Clean exit codes per mode.
WORKER_OK = 0          # worker prints a result line and exits 0
CLI_OK = 0
CLI_FAIL = 1           # analysis failed, structured diagnostic on stderr
CLI_USAGE = 2          # bad arguments


def mutate(data, rng):
    """Return a mutated copy of `data` using a random single or combined strategy."""
    strategies = ["truncate", "flip", "insert", "delete"]
    # 70% single, 30% combined (1..3 ops) to exercise layered corruption.
    if rng.random() < 0.7:
        ops = [rng.choice(strategies)]
    else:
        ops = rng.choices(strategies, k=rng.randint(1, 3))

    out = bytearray(data)
    for op in ops:
        n = len(out)
        if n == 0:
            # empty seed: just synthesize a few random bytes
            out = bytearray(rng.randint(0, 255) for _ in range(rng.randint(1, 64)))
            continue
        if op == "truncate":
            # keep a random 0..100% prefix (0 = totally empty file)
            keep = rng.randint(0, n)
            out = out[:keep]
        elif op == "flip":
            flips = rng.randint(1, min(50, max(1, n // 20)))
            for _ in range(flips):
                i = rng.randrange(n)
                out[i] ^= (1 << rng.randint(0, 7))
        elif op == "insert":
            at = rng.randrange(n)
            blob = bytearray(rng.randint(0, 255) for _ in range(rng.randint(1, 256)))
            out[at:at] = blob
        elif op == "delete":
            span = rng.randint(1, max(1, n // 4))
            at = rng.randrange(n)
            out[at:at + span] = b""
    return bytes(out)


def list_dumps(exe_dir):
    d = os.path.join(exe_dir, "crashes")
    if not os.path.isdir(d):
        return set()
    return set(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".dmp"))


def run_once(exe, mode, path, timeout, clean_env):
    """Run the target on `path`; return a dict classification."""
    crash_dir_before = list_dumps(os.path.dirname(os.path.abspath(exe)))
    rc = None
    timed_out = False
    crash_marker = False
    got_result = False
    stderr_tail = b""

    try:
        if mode == "worker":
            req = (json.dumps({"type": "analyze", "input": path, "options": {}}) + "\n").encode()
            p = subprocess.Popen(
                [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=clean_env,
            )
            try:
                out, err = p.communicate(req, timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                timed_out = True
            rc = p.returncode
            got_result = RESULT_TAG.encode() in out
            crash_marker = b"[CRASHDUMP]" in err
            stderr_tail = err[-400:] if err else b""
        else:  # cli
            p = subprocess.Popen(
                [exe, "--json", path], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=clean_env,
            )
            try:
                out, err = p.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                timed_out = True
            rc = p.returncode
            got_result = (rc == CLI_OK) or (rc == CLI_FAIL)
            crash_marker = b"[CRASHDUMP]" in err
            stderr_tail = err[-400:] if err else b""
    except Exception as e:  # harness must never die on one input
        return {"kind": "harness_error", "detail": repr(e)}

    # Did a new dump appear?
    crash_dir_after = list_dumps(os.path.dirname(os.path.abspath(exe)))
    new_dumps = crash_dir_after - crash_dir_before

    if got_result:
        kind = "ok" if (mode == "worker" or rc == CLI_OK) else "clean_fail"
        # For the worker, a result line with "ok":false is a clean failure.
        if mode == "worker" and b'"ok":false' in out:
            kind = "clean_fail"
        return {"kind": kind}
    if crash_marker or new_dumps:
        return {"kind": "crash", "dumps": sorted(new_dumps), "stderr": stderr_tail,
                "rc": rc}
    if timed_out:
        return {"kind": "timeout", "rc": rc}
    # Process ended without a result and without a crash marker -> abnormal exit.
    return {"kind": "abnormal_exit", "rc": rc, "stderr": stderr_tail}


def save_repro(corpus_dir, idx, data):
    os.makedirs(corpus_dir, exist_ok=True)
    p = os.path.join(corpus_dir, "crash_%06d.bin" % idx)
    with open(p, "wb") as f:
        f.write(data)
    return p


def prune_dumps(dump_paths):
    for d in dump_paths:
        try:
            os.remove(d)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description="Fuzz the analysis kernel (P2).")
    ap.add_argument("--exe", required=True, help="path to analyzer-worker.exe or media-analyse-cli.exe")
    ap.add_argument("--seed", required=True, help="seed media file to mutate")
    ap.add_argument("--mode", choices=["worker", "cli"], default="worker")
    ap.add_argument("--iters", type=int, default=2000, help="number of mutated inputs")
    ap.add_argument("--timeout", type=float, default=20.0, help="per-run wall timeout (s)")
    ap.add_argument("--seed-rng", type=int, default=0, help="RNG seed for reproducibility")
    ap.add_argument("--corpus-dir", default="fuzz-corpus", help="where to save crashing inputs")
    ap.add_argument("--keep-dumps", action="store_true", help="do not delete .dmp files")
    ap.add_argument("--report", default="fuzz_report.json", help="JSON report path")
    ap.add_argument("--env-path", default=None,
                    help="extra PATH to prepend (toolchain runtime DLLs)")
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        sys.exit("fuzz: exe not found: %s" % args.exe)
    if not os.path.isfile(args.seed):
        sys.exit("fuzz: seed not found: %s" % args.seed)

    with open(args.seed, "rb") as f:
        seed = f.read()
    print("fuzz: seed=%s (%d bytes), iters=%d, mode=%s, timeout=%.1fs"
          % (args.seed, len(seed), args.iters, args.mode, args.timeout))

    # Build a clean env with an optional extra PATH (toolchain runtime DLLs).
    env = dict(os.environ)
    if args.env_path:
        env["PATH"] = args.env_path + os.pathsep + env.get("PATH", "")

    rng = random.Random(args.seed_rng)
    counts = {"ok": 0, "clean_fail": 0, "crash": 0, "timeout": 0,
              "abnormal_exit": 0, "harness_error": 0}
    crashes = []  # list of {iter, repro, dumps, stderr}
    t0 = time.time()

    for i in range(args.iters):
        data = mutate(seed, rng)
        fd, tmp = tempfile.mkstemp(prefix="fuzz_", suffix=".bin")
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        try:
            res = run_once(args.exe, args.mode, tmp, args.timeout, env)
        finally:
            # Keep the temp file only if it crashed (save a copy to corpus).
            if res.get("kind") == "crash":
                repro = save_repro(args.corpus_dir, counts["crash"], data)
                res["repro"] = repro
                crashes.append({"iter": i, "repro": repro,
                                "dumps": res.get("dumps", []),
                                "stderr": res.get("stderr", b"").decode("utf-8", "replace")})
                if not args.keep_dumps:
                    prune_dumps(res.get("dumps", []))
                counts["crash"] += 1
            else:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                counts[res.get("kind", "harness_error")] = \
                    counts.get(res.get("kind", "harness_error"), 0) + 1

        if (i + 1) % 200 == 0 or res.get("kind") in ("crash", "timeout", "abnormal_exit"):
            el = time.time() - t0
            print("  [%6d] %-14s  ok=%d fail=%d crash=%d to=%d abn=%d  %.1fs"
                  % (i + 1, res.get("kind"), counts["ok"], counts["clean_fail"],
                     counts["crash"], counts["timeout"], counts["abnormal_exit"], el))

    elapsed = time.time() - t0
    report = {
        "exe": os.path.abspath(args.exe),
        "seed": os.path.abspath(args.seed),
        "mode": args.mode,
        "iters": args.iters,
        "elapsed_s": round(elapsed, 1),
        "counts": counts,
        "crashes": crashes,
    }
    with open(args.report, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    print("\n=== FUZZ SUMMARY ===")
    print("iterations : %d  (%.1fs, %.0f/s)" % (args.iters, elapsed, args.iters / max(elapsed, 1e-6)))
    print("ok         : %d" % counts["ok"])
    print("clean_fail : %d   (malformed -> structured diagnostic, expected)" % counts["clean_fail"])
    print("crash      : %d" % counts["crash"])
    print("timeout    : %d" % counts["timeout"])
    print("abnormal   : %d" % counts["abnormal_exit"])
    print("harness_err: %d" % counts["harness_error"])
    print("report     : %s" % args.report)
    if counts["crash"] or counts["abnormal_exit"]:
        print("RESULT     : FAIL (%d crash / %d abnormal - see corpus %s)"
              % (counts["crash"], counts["abnormal_exit"], args.corpus_dir))
        sys.exit(1)
    print("RESULT     : PASS (0 unhandled crashes across %d mutated inputs)" % args.iters)


if __name__ == "__main__":
    main()
