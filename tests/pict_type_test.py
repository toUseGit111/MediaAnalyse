#!/usr/bin/env python3
"""Regression for I/P/B frame types in the frame table.

Two bugs are guarded here:

1. The old AVCodecParserContext path emitted the *previously* completed frame
   and kept pict_type until the next frame was recognised. Copying it onto the
   current packet row produced three "I" frames at startup and two at every
   GOP start, while the real IDR packet was labelled "P".
2. The first fix over-corrected by removing I/P/B entirely (key frames only).
   That loses the P/B information users actually need.

The current implementation reads each packet's own slice header synchronously
(H.264/H.265 slice_type, MPEG-1/2 picture_coding_type), so types are final when
a live snapshot row is published. AV_PKT_FLAG_KEY remains an authoritative
override for random-access points.

Usage:
    python tests/pict_type_test.py [video_or_url]
    python tests/pict_type_test.py build/rtsp_4s.ts --frames 400
    python tests/pict_type_test.py rtsp://host/live --stall-timeout 5
    python tests/pict_type_test.py some.mp4 --ffprobe   # optional cross-check

MA_CLI can point at a non-default executable. Pure standard library.
"""
import argparse
import collections
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(ROOT, "build", "win-mingw", "cli", "media-analyse-cli.exe")
DEFAULT_VIDEO = os.path.join(ROOT, "tests", "assets", "sample.mp4")
INTRA_CODECS = ("mjpeg", "prores", "dnxhd", "dnxhr", "rawvideo", "huffyuv",
                "ffv1", "intra", "png", "jpeg2000")
VALID = (None, "I", "P", "B")


def run(cli, args, timeout):
    proc = subprocess.run([cli] + args, capture_output=True, text=True,
                          errors="replace", timeout=timeout)
    try:
        doc = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError("invalid JSON: %s (stderr=%s)" %
                             (exc, proc.stderr[-200:])) from exc
    assert doc.get("ok") is True, (proc.returncode, doc.get("error"))
    return doc


def ffprobe_hist(path):
    cmd = ["ffprobe", "-v", "error", "-select_streams", "v:0",
           "-show_frames", "-show_entries", "frame=pict_type",
           "-of", "csv=p=0", path]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace", timeout=180)
    if proc.returncode != 0:
        raise AssertionError("ffprobe failed: %s" % proc.stderr[-200:])
    return collections.Counter(
        line.strip().split(",")[0].strip()
        for line in proc.stdout.splitlines()
        if line.strip() and line.strip().split(",")[0].strip() in ("I", "P", "B")
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?", default=DEFAULT_VIDEO)
    ap.add_argument("--cli", default=os.environ.get("MA_CLI", DEFAULT_CLI))
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--stall-timeout", type=float, default=5.0)
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="hard wall-clock limit for each CLI invocation")
    ap.add_argument("--skip-decode", action="store_true")
    ap.add_argument("--ffprobe", action="store_true",
                    help="cross-check the I/P/B histogram with ffprobe "
                         "(local file only; ffprobe must be on PATH)")
    args = ap.parse_args()

    if not os.path.isfile(args.cli):
        print("CLI not found: %s" % args.cli)
        return 2

    base = ["--json", "--frames=%d" % args.frames,
            "--max-frames", str(args.frames),
            "--stall-timeout", str(args.stall_timeout)]
    results = []

    def check(name, fn):
        try:
            detail = fn()
            print("PASS  %-30s %s" % (name, detail))
            return True
        except Exception as exc:
            print("FAIL  %-30s %s" % (name, exc))
            return False

    plain = run(args.cli, base + [args.input], args.timeout)
    vidx = {s["index"] for s in plain.get("streams", [])
            if s["kind"] == "video"}
    codecs = [s.get("codec", "") for s in plain.get("streams", [])
              if s["kind"] == "video"]
    vf = [f for f in plain.get("frames", []) if f["stream"] in vidx]
    keys = [f for f in vf if f["key"]]
    hist = collections.Counter(f.get("pictType") for f in vf
                               if f.get("pictType") in ("I", "P", "B"))
    intra = any(c in " ".join(codecs) for c in INTRA_CODECS)

    def video_present():
        assert vidx and vf, "no video stream or video rows"
        return "streams=%s rows=%d" % (codecs, len(vf))
    results.append(check("video stream present", video_present))

    def key_rows_are_I():
        bad = [(f["seq"], f.get("pictType")) for f in keys
               if f.get("pictType") != "I"]
        assert not bad, "keyframe not labelled I: %s" % bad[:6]
        return "%d/%d keyframes labelled I" % (len(keys), len(keys))
    results.append(check("keyframes labelled I", key_rows_are_I))

    def labels_are_sane():
        bad = [(f["seq"], f.get("pictType")) for f in vf
               if f.get("pictType") not in VALID]
        assert not bad, "unexpected labels: %s" % bad[:6]
        return "only I/P/B/null on %d rows" % len(vf)
    results.append(check("no invented type labels", labels_are_sane))

    def pb_preserved():
        nonkey = [f for f in vf if not f["key"]]
        typed = [f for f in nonkey if f.get("pictType") in ("P", "B")]
        if intra:
            return "intra-only codec, skipped"
        assert len(nonkey) >= 3, "not enough non-key rows to judge"
        ratio = len(typed) / float(len(nonkey))
        assert ratio >= 0.8, ("P/B missing on non-key rows: %d/%d (%.0f%%)" %
                              (len(typed), len(nonkey), ratio * 100))
        return "%d/%d non-key rows keep P/B, hist=%s" % (
            len(typed), len(nonkey), dict(hist))
    results.append(check("P/B preserved", pb_preserved))

    def startup_not_stale():
        # The parser bug showed three consecutive I packets at stream start.
        if intra:
            return "intra-only codec, skipped"
        head = [f.get("pictType") for f in vf[:3]]
        assert head.count("I") <= 1, "startup I burst: %s" % head
        if vf and keys:
            pos = next((i for i, f in enumerate(vf) if f["key"]), None)
            if pos is not None and pos + 1 < len(vf):
                assert vf[pos + 1].get("pictType") in ("P", "B", None), (
                    "row after first IDR is %s" % vf[pos + 1].get("pictType"))
        return "head=%s" % head
    results.append(check("no startup I burst", startup_not_stale))

    def no_consecutive_I():
        pos = [i for i, f in enumerate(vf) if f.get("pictType") == "I"]
        if intra:
            return "intra-only codec, skipped"
        gaps = [b - a for a, b in zip(pos, pos[1:])]
        assert all(g > 1 for g in gaps), "adjacent I frames at %s" % pos[:8]
        return "I positions=%s gaps=%s" % (pos[:8], gaps[:8])
    results.append(check("no consecutive I frames", no_consecutive_I))

    def gop_interior():
        if intra or len(keys) < 2:
            return "not enough GOPs, skipped"
        pos = [i for i, f in enumerate(vf) if f["key"]]
        bad = []
        for a, b in zip(pos, pos[1:]):
            bad += [vf[i]["seq"] for i in range(a + 1, b)
                    if vf[i].get("pictType") == "I"]
        assert not bad, "extra I inside a GOP at seq %s" % bad[:6]
        return "%d GOPs, all interiors are P/B" % (len(pos) - 1)
    results.append(check("one I per GOP", gop_interior))

    if args.ffprobe:
        def cross_check():
            assert os.path.isfile(args.input), "--ffprobe needs a local file"
            ref = ffprobe_hist(args.input)
            # Compare counts, not order: B-frame reordering may shuffle the
            # tail but cannot change how many of each type were decoded.
            for t in ("I", "P", "B"):
                ours, theirs = hist.get(t, 0), ref.get(t, 0)
                assert abs(ours - theirs) <= max(2, int(theirs * 0.02)), (
                    "%s count mismatch: analyser=%d ffprobe=%d" %
                    (t, ours, theirs))
            return "ours=%s ffprobe=%s" % (dict(hist), dict(ref))
        results.append(check("ffprobe histogram agrees", cross_check))

    if not args.skip_decode:
        def decode_is_additive():
            dec = run(args.cli, base + ["--decode", args.input], args.timeout)
            a = [f.get("pictType") for f in plain["frames"]]
            b = [f.get("pictType") for f in dec["frames"]]
            assert a == b, "decode changed type sequence (B-frame shift?)"
            return "%d rows identical with --decode" % len(a)
        results.append(check("decode keeps types", decode_is_additive))
    else:
        print("SKIP  %-30s --skip-decode" % "decode keeps types")

    ok = all(results)
    print("\n%d/%d checks passed" % (sum(results), len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
