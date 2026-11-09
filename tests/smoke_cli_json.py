#!/usr/bin/env python3
"""Regression smoke test for media-analyse-cli --json output.

Pure standard library. Build the CLI first, then:

    python tests/smoke_cli_json.py [optional_video.mp4]

Guards the bug where --frames=N (N < total frames) emitted a trailing comma
in the "frames" array, producing unparseable JSON. Also covers the
success / no-frames / failure paths and, when a video file is supplied,
synchronous I/P/B slice types and audio sample counts.

Additionally guards the decode-path contract: --decode must be purely
additive (frame-level extras only, identical packet rows), must not shift
I/P/B against pts, must honour --max-decode-frames, and - importantly -
must KEEP the extras on the head rows once that budget is exhausted
(per stream), instead of dropping all decode information.

Use MA_CLI to point at a non-default executable path.
"""
import json
import os
import subprocess
import sys
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(ROOT, "build", "win-mingw", "cli", "media-analyse-cli.exe")


def run(cli, args):
    proc = subprocess.run([cli] + args, capture_output=True, text=True,
                          errors="replace", timeout=60)
    try:
        doc = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError("invalid JSON: %s" % exc) from exc
    return proc.returncode, doc


def make_wav(path):
    # 1 s of silence, mono 8 kHz 16-bit PCM - enough packets to exercise
    # --frames=N with N smaller than the total.
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(b"\x00\x00" * 8000)


def check(name, fn):
    try:
        detail = fn()
        print("PASS  %-24s %s" % (name, detail))
        return True
    except Exception as exc:
        print("FAIL  %-24s %s" % (name, exc))
        return False


def main():
    cli = os.environ.get("MA_CLI", DEFAULT_CLI)
    if not os.path.isfile(cli):
        print("CLI not found: %s (build first, or set MA_CLI)" % cli)
        return 2
    video = sys.argv[1] if len(sys.argv) > 1 else ""
    results = []

    with tempfile.TemporaryDirectory(prefix="ma-smoke-") as tmp:
        wav = os.path.join(tmp, "tone.wav")
        junk = os.path.join(tmp, "junk.bin")
        missing = os.path.join(tmp, "missing.mp4")
        make_wav(wav)
        with open(junk, "wb") as f:
            f.write(os.urandom(4096))

        def limited():
            rc, d = run(cli, ["--json", "--frames=3", wav])
            assert rc == 0 and d["ok"] is True, (rc, d.get("error"))
            assert len(d["frames"]) == 3, len(d.get("frames", []))
            return "frames=%d, parseable" % len(d["frames"])
        results.append(check("limited --frames=N", limited))

        def all_frames():
            rc, d = run(cli, ["--json", "--frames", wav])
            assert rc == 0 and d["ok"] is True, (rc, d.get("error"))
            return "frames=%d" % len(d["frames"])
        results.append(check("unlimited --frames", all_frames))

        def no_frames():
            rc, d = run(cli, ["--json", wav])
            assert rc == 0 and "frames" not in d, (rc, list(d))
            return "no frames key, ok=%s" % d["ok"]
        results.append(check("no --frames", no_frames))

        def failure_junk():
            rc, d = run(cli, ["--json", "--frames=3", junk])
            assert rc == 1 and d["ok"] is False, (rc, d.get("error"))
            assert d["error"]["native"] != 0, d["error"]
            return "error=%s/%s native=%s" % (d["error"]["code"],
                                              d["error"]["stage"],
                                              d["error"]["native"])
        results.append(check("damaged file", failure_junk))

        def failure_missing():
            rc, d = run(cli, ["--json", "--frames=3", missing])
            assert rc == 1 and d["ok"] is False, (rc, d.get("error"))
            return "error=%s" % d["error"]["code"]
        results.append(check("missing file", failure_missing))

        def finite_numbers():
            # NaN/Infinity are not JSON literals; jnum() must emit null.
            _, d = run(cli, ["--json", wav])
            for key in ("duration", "start"):
                val = d["format"][key]
                assert val is None or isinstance(val, (int, float)), (key, val)
            return "format duration/start finite or null"
        results.append(check("finite JSON numbers", finite_numbers))

        def decode_is_additive():
            # Decoding must never change the packet-level table. This is the
            # regression guard for the B-frame reorder bug that used to pair
            # fr->pict_type with a different packet's pts ("IPIBPB..." shift).
            _, plain = run(cli, ["--json", "--frames", wav])
            _, dec = run(cli, ["--json", "--frames", "--decode", wav])
            a, b = plain["frames"], dec["frames"]
            keys = ("seq", "stream", "dts", "pts", "pictType", "samples", "size")
            assert len(a) == len(b), (len(a), len(b))
            for x, y in zip(a, b):
                for k in keys:
                    assert x[k] == y[k], (k, x[k], y[k])
            assert all(f["decoded"] is False for f in a), "plain run has decoded rows"
            assert any(f["decoded"] for f in b), "no row marked decoded"
            assert any(f.get("sampleFmt") for f in b), "no audio sample format"
            assert any(f.get("channels") for f in b), "no channel count"
            return "rows=%d identical, extras only" % len(b)
        results.append(check("decode is additive", decode_is_additive))

        if video:
            def slice_types():
                # Two regressions in one: the delayed AVCodecParser bug
                # ("3 I at startup, 2 per GOP") AND the over-correction that
                # removed P/B entirely. Key frames must be "I", and ordinary
                # frames must keep the real I/P/B read synchronously from
                # their own slice header - no parser, no decoder, no lag.
                rc, d = run(cli, ["--json", "--frames=40", video])
                assert rc == 0 and d["ok"] is True, (rc, d.get("error"))
                vidx = {s["index"] for s in d["streams"] if s["kind"] == "video"}
                codecs = [s.get("codec", "") for s in d["streams"]
                          if s["kind"] == "video"]
                vf = [f for f in d["frames"] if f["stream"] in vidx]
                assert vf, "no video rows"
                for f in vf:
                    assert f.get("pictType") in (None, "I", "P", "B"), (
                        f["seq"], f.get("pictType"))
                    if f["key"]:
                        assert f.get("pictType") == "I", (
                            f["seq"], "keyframe not labelled I",
                            f.get("pictType"))
                assert any(f["key"] for f in vf), "no keyframe in first 40 rows"
                nonkey = [f for f in vf if not f["key"]]
                typed = [f for f in nonkey
                         if (f.get("pictType") or "") in ("I", "P", "B")]
                intra = any(c in " ".join(codecs) for c in
                            ("mjpeg", "prores", "dnxhd", "rawvideo", "ffv1",
                             "huffyuv", "png", "jpeg2000"))
                if not intra and len(nonkey) >= 3:
                    assert typed, ("no P/B labels on non-key frames; "
                                   "slice reader is not running")
                samples = [f["samples"] for f in d["frames"]
                           if f["stream"] not in vidx]
                return "video=%d keys=%d p/b=%d pict=%s samples=%s" % (
                    len(vf), sum(1 for f in vf if f["key"]), len(typed),
                    [f.get("pictType") for f in vf[:8]], samples[:3])
            results.append(check("slice types", slice_types))

            def decode_keeps_types():
                # Decode stays purely additive: I/P/B already comes from the
                # packet's own slice header, so decoder output (B-frame
                # reordering) must never rewrite the type sequence.
                _, plain = run(cli, ["--json", "--frames=60", video])
                _, dec = run(cli, ["--json", "--frames=60", "--decode", video])
                ta = [f.get("pictType") for f in plain["frames"]]
                tb = [f.get("pictType") for f in dec["frames"]]
                assert ta == tb, "decode changed type sequence (B-frame shift?)"
                return "types identical (%d rows)" % len(ta)
            results.append(check("decode keeps types", decode_keeps_types))

            def decode_extras():
                _, d = run(cli, ["--json", "--frames=40", "--decode", video])
                vidx = {s["index"] for s in d["streams"] if s["kind"] == "video"}
                vf = [f for f in d["frames"] if f["stream"] in vidx and f["decoded"]]
                assert vf and vf[0]["width"] and vf[0]["pixFmt"], vf[:1]
                return "%dx%d pix=%s" % (vf[0]["width"], vf[0]["height"],
                                         vf[0]["pixFmt"])
            results.append(check("decode frame extras", decode_extras))

            def decode_budget():
                _, d = run(cli, ["--json", "--frames", "--decode",
                                 "--max-decode-frames", "20", video])
                marks = [f["decoded"] for f in d["frames"]]
                assert any(marks) and not all(marks), (
                    marks.count(True), len(marks))
                return "decoded=%d of %d rows" % (marks.count(True), len(marks))
            results.append(check("decode budget cap", decode_budget))

            def decode_budget_keeps_head():
                # Regression for "exceeding the budget hid ALL decode info":
                # the rows that were decoded must survive and stay at the top.
                _, d = run(cli, ["--json", "--frames", "--decode",
                                 "--max-decode-frames", "20", video])
                fr = d["frames"]
                idx = [i for i, f in enumerate(fr) if f["decoded"]]
                assert idx, "no decoded row survived"
                assert max(idx) < 120, (
                    "extras are not on the head rows, last decoded index=%d"
                    % max(idx))
                head = [f for f in fr[:idx[-1] + 1] if f["decoded"]]
                assert all(f.get("width") or f.get("sampleFmt") for f in head), (
                    "a decoded head row has no frame-level extras")
                return "head rows seq %d..%d keep extras" % (
                    fr[idx[0]]["seq"], fr[idx[-1]]["seq"])
            results.append(check("decode budget keeps head",
                                 decode_budget_keeps_head))

            def decode_budget_per_stream():
                # An audio-heavy container must not eat the whole quota and
                # leave the video tab completely empty.
                _, d = run(cli, ["--json", "--frames", "--decode",
                                 "--max-decode-frames", "20", video])
                vidx = {s["index"] for s in d["streams"] if s["kind"] == "video"}
                by_stream = {}
                for f in d["frames"]:
                    if f["decoded"]:
                        by_stream[f["stream"]] = by_stream.get(f["stream"], 0) + 1
                assert by_stream, "no decoded rows"
                assert any(s in vidx for s in by_stream), (
                    "video stream got no decode extras: %s" % by_stream)
                assert all(n <= 25 for n in by_stream.values()), by_stream
                return "per-stream decoded rows %s" % by_stream
            results.append(check("decode budget per stream",
                                 decode_budget_per_stream))

            def decode_budget_hint():
                # The UI must be able to tell the user that the first N rows
                # DO carry extras; keep that wording in the JSON notes too.
                _, d = run(cli, ["--json", "--frames", "--decode",
                                 "--max-decode-frames", "20", video])
                notes = d.get("notes", "")
                assert "ARE shown on the first" in notes, notes
                assert "per stream" in notes, notes
                return "notes: %s" % notes[:110]
            results.append(check("decode budget hint", decode_budget_hint))
        else:
            for name in ("slice types", "decode keeps types",
                         "decode frame extras", "decode budget cap",
                         "decode budget keeps head", "decode budget per stream",
                         "decode budget hint"):
                print("SKIP  %-24s pass a video file to enable" % name)

    ok = all(results)
    print("\n%d/%d checks passed" % (sum(results), len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
