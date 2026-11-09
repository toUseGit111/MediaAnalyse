#!/usr/bin/env python3
"""End-to-end regression for HLS timeline / A-V alignment in media-analyse-cli.

The contract being guarded (2026-11-09):
  #EXT-X-PROGRAM-DATE-TIME is OPTIONAL. Recordings without it must still get
  every in-file diagnostic (missing segment, undeclared discontinuity, A/V
  offset); only cross-file ABSOLUTE alignment is downgraded to a clearly
  labelled relative/manual one. When PDT appears, the same binary upgrades to
  a wall-clock timeline without any rebuild or flag.

Samples are the four playlists under build/hls_sample/ (plus their .ts files).
If they are absent the script regenerates them with ffmpeg when available:

    MA_HLS_SAMPLE_DIR   override the sample directory
    MA_FFMPEG           path to ffmpeg.exe (else PATH / common install path)
    MA_CLI              path to media-analyse-cli.exe
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLI = os.path.join(ROOT, "build", "win-mingw", "cli",
                           "media-analyse-cli.exe")
SAMPLE_DIR = os.environ.get("MA_HLS_SAMPLE_DIR",
                            os.path.join(ROOT, "build", "hls_sample"))
PLAYLISTS = ("index.m3u8", "leak.m3u8", "discont.m3u8", "missing.m3u8")


def wpath(p):
    return p.replace("\\", "/")


def find_ffmpeg():
    override = os.environ.get("MA_FFMPEG")
    if override and os.path.isfile(override):
        return override
    found = shutil.which("ffmpeg")
    if found:
        return found
    for cand in (r"D:\CodeTools\ffmpeg-n9.0-latest-win64-gpl-shared-9.0\bin\ffmpeg.exe",
                 r"C:\ffmpeg\bin\ffmpeg.exe"):
        if os.path.isfile(cand):
            return cand
    return ""


def generate_samples():
    """Build a 4 s HLS VOD (with PDT) and three deliberately broken variants."""
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        return False, "ffmpeg not found; set MA_FFMPEG or MA_HLS_SAMPLE_DIR"
    os.makedirs(SAMPLE_DIR, exist_ok=True)
    index = os.path.join(SAMPLE_DIR, "index.m3u8")
    seg_pattern = os.path.join(SAMPLE_DIR, "seg_%03d.ts")
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
           "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=30",
           "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
           "-t", "4", "-c:v", "libx264", "-preset", "ultrafast",
           "-pix_fmt", "yuv420p", "-c:a", "aac",
           "-f", "hls", "-hls_time", "1",
           "-hls_flags", "+program_date_time",
           "-hls_segment_filename", seg_pattern, index]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if proc.returncode != 0 or not os.path.isfile(index):
        return False, "ffmpeg failed: " + proc.stderr.strip()[:200]

    with open(index, "r", encoding="utf-8", errors="replace") as f:
        lines = [ln.rstrip("\r\n") for ln in f]

    # Collect (EXTINF line, PDT line, uri) triples from the generated list.
    triples = []
    pending_extinf, pending_pdt = None, None
    for ln in lines:
        if ln.startswith("#EXTINF:"):
            pending_extinf = ln
        elif ln.startswith("#EXT-X-PROGRAM-DATE-TIME:"):
            pending_pdt = ln
        elif ln and not ln.startswith("#"):
            triples.append((pending_extinf, pending_pdt, ln))
            pending_extinf = pending_pdt = None
    if len(triples) < 3:
        return False, "generated playlist has fewer than 3 segments"

    def write(name, chosen, declared=False, drop_pdt=True):
        out = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2",
               "#EXT-X-MEDIA-SEQUENCE:0"]
        for k, (extinf, pdt, uri) in enumerate(chosen):
            if declared and k == 1:
                out.append("#EXT-X-DISCONTINUITY")
            if extinf:
                out.append(extinf)
            if pdt and not drop_pdt:
                out.append(pdt)
            out.append(uri)
        out.append("#EXT-X-ENDLIST")
        with open(os.path.join(SAMPLE_DIR, name), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write("\n".join(out) + "\n")

    # Same two physical segments, but the second one is 2 s of media under a
    # 1 s EXTINF: that is the "undeclared discontinuity" case from the field.
    write("leak.m3u8", [triples[0], triples[2]], declared=False)
    write("discont.m3u8", [triples[0], triples[2]], declared=True)
    missing = (triples[0][0], triples[0][1], "seg_999.ts")
    write("missing.m3u8", [triples[0], missing], declared=False)
    return True, "generated"


def ensure_samples():
    needed = [os.path.join(SAMPLE_DIR, p) for p in PLAYLISTS]
    tss = [os.path.join(SAMPLE_DIR, "seg_%03d.ts" % i) for i in range(4)]
    if all(os.path.isfile(p) for p in needed + tss[:3]):
        return True, "existing"
    return generate_samples()


def run_json(cli, args):
    proc = subprocess.run([cli] + args, capture_output=True, text=True,
                          errors="replace", timeout=120)
    try:
        doc = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError("invalid JSON (%s): %s" % (exc, proc.stdout[:200]))
    return proc.returncode, doc


def codes(doc, severity=None):
    out = []
    for x in doc.get("alignment", {}).get("issues", []):
        if severity is None or x.get("severity") == severity:
            out.append(x.get("code"))
    return out


def check(name, fn):
    try:
        detail = fn()
        print("PASS  %-38s %s" % (name, detail))
        return True
    except Exception as exc:
        print("FAIL  %-38s %s" % (name, exc))
        return False


def main():
    cli = os.environ.get("MA_CLI", DEFAULT_CLI)
    if not os.path.isfile(cli):
        print("CLI not found: %s (build first, or set MA_CLI)" % cli)
        return 2
    ok, info = ensure_samples()
    if not ok:
        print("samples unavailable: %s" % info)
        return 2
    print("samples: %s (%s)" % (SAMPLE_DIR, info))
    results = []
    P = lambda name: wpath(os.path.join(SAMPLE_DIR, name))

    def pdt_timeline():
        rc, d = run_json(cli, ["--json", "--frames=8", P("index.m3u8")])
        assert rc == 0 and d["ok"] is True, (rc, d.get("error"))
        a = d["alignment"]
        assert a["timeline"] == "program-date-time", a["timeline"]
        assert a["crossFileComparable"] is True
        pl = a["playlist"]
        assert pl["pdtCount"] == len(pl["segments"]) == 4, pl["pdtCount"]
        assert all(s["opened"] for s in pl["segments"]), pl["segments"]
        assert a["firstWallClock"] and a["lastWallClock"]
        assert "error" not in [x["severity"] for x in a["issues"]], a["issues"]
        return "timeline=%s PDT=%d wall=%s" % (
            a["timeline"], pl["pdtCount"], a["firstWallClock"])
    results.append(check("PDT playlist -> absolute timeline", pdt_timeline))

    def pdt_frames():
        _, d = run_json(cli, ["--json", "--frames=40", P("index.m3u8")])
        fr = d["frames"]
        wall = [f["wallClockMs"] for f in fr if f.get("wallClockMs") is not None]
        assert wall, "no frame carries wallClockMs"
        assert any(f["segment"] >= 0 for f in fr), "no frame segment index"
        assert all(isinstance(x, int) for x in wall)
        return "rows=%d wallClock=%d segments=%s" % (
            len(fr), len(wall), sorted({f["segment"] for f in fr}))
    results.append(check("frames carry segment + wall clock", pdt_frames))

    def no_pdt_still_diagnoses():
        _, d = run_json(cli, ["--json", P("leak.m3u8")])
        a = d["alignment"]
        assert a["timeline"] == "playlist-elapsed", a["timeline"]
        assert a["crossFileComparable"] is False
        assert a["firstWallClockMs"] is None
        assert "timeline-relative" in codes(d), codes(d)
        assert "unmarked-discontinuity" in codes(d, "error"), codes(d)
        assert "stream-gap" not in codes(d), codes(d)
        return "timeline=%s errors=%d warnings=%d" % (
            a["timeline"], a["errorCount"], a["warningCount"])
    results.append(check("no PDT: all in-file checks still work",
                         no_pdt_still_diagnoses))

    def no_pdt_no_wallclock():
        _, d = run_json(cli, ["--json", "--frames=8", P("leak.m3u8")])
        assert all(f.get("wallClockMs") is None for f in d["frames"]), \
            "wall clock invented without PDT"
        return "rows=%d no fabricated wall clock" % len(d["frames"])
    results.append(check("no PDT: no fabricated wall clock",
                         no_pdt_no_wallclock))

    def declared():
        _, d = run_json(cli, ["--json", P("discont.m3u8")])
        c = codes(d)
        assert "declared-discontinuity" in c, c
        assert "unmarked-discontinuity" not in c, c
        assert d["alignment"]["errorCount"] == 0, c
        return "declared discontinuity, 0 errors"
    results.append(check("declared discontinuity is not an error", declared))

    def missing():
        _, d = run_json(cli, ["--json", P("missing.m3u8")])
        assert "segment-missing" in codes(d, "error"), codes(d)
        segs = d["alignment"]["playlist"]["segments"]
        assert any(not s["opened"] for s in segs), segs
        return "segment-missing error raised (diag ok=%s)" % d["ok"]
    results.append(check("missing segment is no longer silent", missing))

    def no_align():
        _, d = run_json(cli, ["--json", "--no-align", P("index.m3u8")])
        assert "alignment" not in d, list(d)
        return "alignment omitted"
    results.append(check("--no-align skips post-processing", no_align))

    def compare_pdt():
        _, d = run_json(cli, ["--json", "--compare", P("index.m3u8"),
                              P("index.m3u8")])
        c = d["comparison"]
        assert c["method"] == "pdt" and c["comparable"] is True, c
        assert c["confidence"] == 1.0 and abs(c["offsetMs"]) < 1e-6, c
        return "pdt offset=%.1f ms confidence=%.2f" % (
            c["offsetMs"], c["confidence"])
    results.append(check("--compare with PDT is exact", compare_pdt))

    def compare_relative():
        _, d = run_json(cli, ["--json", "--compare", P("index.m3u8"),
                              P("leak.m3u8")])
        c = d["comparison"]
        assert c["method"] == "relative" and c["confidence"] == 0.0, c
        assert "PDT" in c["note"], c["note"]
        return "relative, confidence=0, explicitly labelled"
    results.append(check("--compare without PDT is honest", compare_relative))

    def compare_anchor():
        _, d = run_json(cli, ["--json", "--compare=" + P("index.m3u8"),
                              "--anchor-ms=1500", P("leak.m3u8")])
        c = d["comparison"]
        assert c["method"] == "manual-anchor" and c["confidence"] == 0.5, c
        assert abs(c["offsetMs"] - 1500.0) < 1e-6, c
        return "manual anchor 1500 ms, confidence=0.5"
    results.append(check("--anchor-ms supplies a manual offset", compare_anchor))

    def worker_wire():
        # GUI path: the crash-isolated worker must carry the same alignment
        # report through the line-delimited JSON protocol.
        worker = os.environ.get("MA_WORKER", os.path.join(
            ROOT, "build", "win-mingw", "worker", "analyzer-worker.exe"))
        assert os.path.isfile(worker), "worker not built: %s" % worker
        req = json.dumps({"type": "analyze", "input": P("index.m3u8")}) + "\n"
        proc = subprocess.run([worker], input=req, capture_output=True,
                              text=True, errors="replace", timeout=120)
        docs = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    docs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        finals = [d for d in docs if d.get("type") == "result"]
        assert finals, "worker emitted no result line: %s" % proc.stderr[-300:]
        a = finals[-1].get("alignment")
        assert a and a["timeline"] == "program-date-time", a
        assert a["crossFileComparable"] is True
        return "worker result line carries PDT alignment"
    results.append(check("worker IPC carries alignment report", worker_wire))

    def non_hls():
        # A plain WAV must not gain fake HLS segments or alarms.
        import tempfile
        import wave
        with tempfile.TemporaryDirectory(prefix="ma-align-") as tmp:
            wav = os.path.join(tmp, "tone.wav")
            with wave.open(wav, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
                w.writeframes(b"\x00\x00" * 8000)
            _, d = run_json(cli, ["--json", wpath(wav)])
            a = d["alignment"]
            assert a["timeline"] == "pts-only", a["timeline"]
            assert a["playlist"]["segments"] == [], a["playlist"]
            assert a["errorCount"] == 0, a["issues"]
        return "non-HLS degrades to pts-only, no errors"
    results.append(check("non-HLS input degrades gracefully", non_hls))

    print("\n%d/%d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
