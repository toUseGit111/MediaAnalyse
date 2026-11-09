// Unit tests for the OPTIONAL-PDT HLS timeline / A-V alignment model.
// Pure ma::core: no FFmpeg, no Qt, no media files, deterministic.
// The central contract under test: a playlist WITHOUT #EXT-X-PROGRAM-DATE-TIME
// keeps every in-file diagnostic working, while cross-file alignment is
// explicitly downgraded; adding PDT upgrades the same code path to wall clock.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "ma/core/Alignment.hpp"
#include "ma/core/Serialization.hpp"

static int gFailed = 0;
static int gTotal = 0;

static void expect(const char* name, bool ok, const std::string& detail = {}) {
    ++gTotal;
    if (!ok) ++gFailed;
    std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}

static bool hasIssue(const ma::AlignmentReport& r, const char* code,
                     const char* severity = nullptr) {
    for (const auto& x : r.issues)
        if (x.code == code && (!severity || x.severity == severity)) return true;
    return false;
}

static const char* kPdt =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-TARGETDURATION:1\n"
    "#EXT-X-MEDIA-SEQUENCE:7\n"
    "#EXTINF:1.000000,\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-11-09T15:52:48.227+0800\n"
    "seg_000.ts\n"
    "#EXTINF:1.000000,\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-11-09T15:52:49.227+0800\n"
    "seg_001.ts\n"
    "#EXTINF:1.000000,\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-11-09T15:52:50.227+0800\n"
    "seg_002.ts\n"
    "#EXT-X-ENDLIST\n";

static const char* kNoPdt =
    "#EXTM3U\r\n"
    "#EXT-X-TARGETDURATION:2\r\n"
    "#EXTINF:1.000000,\r\n"
    "seg_000.ts\r\n"
    "#EXT-X-DISCONTINUITY\r\n"
    "#EXTINF:1.000000,\r\n"
    "seg_002.ts\r\n"
    "#EXT-X-ENDLIST\r\n";

// Two streams, 30 evenly spaced packets each. Audio starts at the same time
// unless `audioOffsetSeconds` is given. `clockSkew` makes every declared
// duration smaller than the real pts increment (systematic clock drift).
static ma::AnalysisResult makeResult(double audioOffsetSeconds = 0.0,
                                     double clockSkew = 0.0) {
    ma::AnalysisResult r;
    ma::StreamInfo v;
    v.index = 0; v.kind = ma::StreamKind::Video; v.codecName = "h264";
    ma::StreamInfo a;
    a.index = 1; a.kind = ma::StreamKind::Audio; a.codecName = "aac";
    r.streams = {v, a};
    for (int i = 0; i < 30; ++i) {
        ma::FrameRecord f;
        f.seq = i + 1; f.streamIndex = 0;
        f.ptsTime = i * 0.033; f.dtsTime = f.ptsTime;
        f.durationTime = 0.033 - clockSkew;
        r.frames.push_back(f);
    }
    for (int i = 0; i < 30; ++i) {
        ma::FrameRecord f;
        f.seq = 31 + i; f.streamIndex = 1;
        f.ptsTime = audioOffsetSeconds + i * 0.023; f.dtsTime = f.ptsTime;
        f.durationTime = 0.023 - clockSkew;
        r.frames.push_back(f);
    }
    return r;
}

// Mark all segments as opened, with plausible per-segment measured pts.
static void markOpened(ma::HlsPlaylist& pl) {
    for (auto& s : pl.segments) {
        s.opened = true;
        s.packetCount = 20;
        s.firstPtsSeconds = s.playlistStartSeconds;
        s.lastPtsSeconds = s.playlistStartSeconds + 0.95;
        s.firstPtsByStream[0] = s.playlistStartSeconds;
        s.firstPtsByStream[1] = s.playlistStartSeconds;
    }
}

int main() {
    using namespace ma;

    // ---- ISO-8601 parsing --------------------------------------------------
    const auto utc = parseIso8601ToEpochMs("2026-11-09T07:52:48.227Z");
    const auto plus0800 = parseIso8601ToEpochMs("2026-11-09T15:52:48.227+0800");
    const auto plus0800c = parseIso8601ToEpochMs("2026-11-09T15:52:48.227+08:00");
    expect("ISO8601 Z parsed", utc.has_value());
    expect("ISO8601 +0800 == UTC", plus0800 && utc && *plus0800 == *utc);
    expect("ISO8601 +08:00 == UTC", plus0800c && utc && *plus0800c == *utc);
    expect("ISO8601 fractional ms", utc &&
           formatEpochMsUtc(*utc) == "2026-11-09T07:52:48.227Z",
           utc ? formatEpochMsUtc(*utc) : "-");
    expect("ISO8601 malformed rejected",
           !parseIso8601ToEpochMs("not-a-date") &&
           !parseIso8601ToEpochMs("2026-13-45T99:99:99Z"));
    expect("ISO8601 no-zone treated as UTC",
           parseIso8601ToEpochMs("2026-11-09T07:52:48Z") ==
           parseIso8601ToEpochMs("2026-11-09T07:52:48"));

    // ---- playlist parsing --------------------------------------------------
    const auto plPdt = parseHlsPlaylist(kPdt, "index.m3u8");
    expect("playlist with PDT: 3 segments", plPdt.segments.size() == 3);
    expect("playlist with PDT: all tagged", plPdt.hasPdt() && plPdt.allPdt());
    expect("playlist cumulative EXTINF",
           std::abs(plPdt.segments[2].playlistStartSeconds - 2.0) < 1e-9 &&
           std::abs(plPdt.totalDuration() - 3.0) < 1e-9);
    expect("playlist media sequence offset",
           plPdt.mediaSequence == 7 && plPdt.segments[2].mediaSequence == 9);
    expect("playlist target duration / endlist",
           std::abs(plPdt.targetDuration - 1.0) < 1e-9 && plPdt.hasEndList);

    const auto plNo = parseHlsPlaylist(kNoPdt, "leak.m3u8");
    expect("CRLF playlist without PDT", plNo.segments.size() == 2 &&
           !plNo.hasPdt() && plNo.pdtCount == 0);
    expect("DISCONTINUITY applies to next segment only",
           !plNo.segments[0].discontinuity && plNo.segments[1].discontinuity);
    const auto master = parseHlsPlaylist(
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2000000\nv.m3u8\n");
    expect("master playlist flagged", master.isMaster);

    // ---- timeline kinds ----------------------------------------------------
    const auto ptsOnly = computeAlignment(makeResult(), HlsPlaylist{});
    expect("no playlist -> PtsOnly", ptsOnly.timeline == TimelineKind::PtsOnly &&
           !ptsOnly.crossFileComparable);

    HlsPlaylist noPdtOpen = plNo;
    markOpened(noPdtOpen);
    const auto relative = computeAlignment(makeResult(), noPdtOpen);
    expect("no PDT -> PlaylistElapsed",
           relative.timeline == TimelineKind::PlaylistElapsed &&
           !relative.crossFileComparable);
    expect("no PDT emits explicit info issue",
           hasIssue(relative, "timeline-relative", "info"));
    expect("no PDT has no wall clock",
           !relative.firstWallClockMs && !relative.lastWallClockMs);

    HlsPlaylist pdtOpen = plPdt;
    markOpened(pdtOpen);
    const auto absolute = computeAlignment(makeResult(), pdtOpen);
    expect("PDT -> ProgramDateTime",
           absolute.timeline == TimelineKind::ProgramDateTime &&
           absolute.crossFileComparable);
    expect("PDT sets wall clock coverage",
           absolute.firstWallClockMs && absolute.lastWallClockMs &&
           *absolute.lastWallClockMs > *absolute.firstWallClockMs);
    expect("clean PDT playlist has no errors", absolute.errorCount == 0,
           std::to_string(absolute.errorCount) + " errors");

    // ---- segment diagnostics ----------------------------------------------
    HlsPlaylist missing = plNo;
    markOpened(missing);
    missing.segments[1].opened = false;
    missing.segments[1].firstPtsSeconds.reset();
    missing.segments[1].lastPtsSeconds.reset();
    const auto missRep = computeAlignment(makeResult(), missing);
    expect("missing segment -> error",
           hasIssue(missRep, "segment-missing", "error") &&
           missRep.errorCount >= 1);

    // An undeclared 1 s pts hole inside segment 1 must be an ERROR, not a
    // silent "ok". Declaring DISCONTINUITY downgrades it to info.
    ma::AnalysisResult holed = makeResult();
    for (auto& f : holed.frames)
        f.segmentIndex = (f.ptsTime && *f.ptsTime < 0.5) ? 0 : 1;
    for (auto& f : holed.frames)
        if (f.ptsTime && *f.ptsTime >= 0.5)
            *f.ptsTime += 1.0, *f.dtsTime += 1.0;
    HlsPlaylist plainOpen = noPdtOpen;                  // no DISCONTINUITY tag
    plainOpen.segments[1].discontinuity = false;
    const auto unmarked = computeAlignment(holed, plainOpen);
    expect("undeclared jump -> unmarked-discontinuity",
           hasIssue(unmarked, "unmarked-discontinuity", "error"));

    // noPdtOpen already declares DISCONTINUITY on segment 1.
    const auto declared = computeAlignment(holed, noPdtOpen);
    expect("declared jump -> info, not error",
           hasIssue(declared, "declared-discontinuity", "info") &&
           !hasIssue(declared, "unmarked-discontinuity"));

    HlsPlaylist pdtGap = pdtOpen;
    pdtGap.segments[2].pdtMs = *pdtGap.segments[2].pdtMs + 2000;
    const auto gapRep = computeAlignment(makeResult(), pdtGap);
    expect("PDT discontinuity -> pdt-gap warning",
           hasIssue(gapRep, "pdt-gap", "warning"));

    HlsPlaylist partialPdt = pdtOpen;
    partialPdt.segments[1].pdtMs.reset();
    partialPdt.pdtCount = 2;
    const auto partialRep = computeAlignment(makeResult(), partialPdt);
    expect("partial PDT -> warning", hasIssue(partialRep, "pdt-partial", "warning"));

    // ---- A/V alignment -----------------------------------------------------
    const auto avOff = computeAlignment(makeResult(0.3), pdtOpen);
    expect("large A/V start offset -> error",
           hasIssue(avOff, "av-offset", "error"));
    const auto avOk = computeAlignment(makeResult(0.01), pdtOpen);
    expect("small A/V start offset -> no issue",
           !hasIssue(avOk, "av-offset"));
    const auto skewed = computeAlignment(makeResult(0.0, 0.003), pdtOpen);
    expect("systematic clock skew -> clock-drift warning",
           hasIssue(skewed, "clock-drift", "warning"));
    const auto clean = computeAlignment(makeResult(), pdtOpen);
    expect("clean stream -> no drift alarm",
           !hasIssue(clean, "clock-drift") &&
           std::abs(clean.streams[0].driftMsPerMin) < 1.0);

    // ---- cross-file comparison ---------------------------------------------
    HlsPlaylist shifted = pdtOpen;
    for (auto& s : shifted.segments)
        if (s.pdtMs) *s.pdtMs += 1500;
    const auto shiftedRep = computeAlignment(makeResult(), shifted);
    const auto cmpPdt = compareTimelines(shiftedRep, absolute);
    expect("PDT vs PDT -> exact offset",
           cmpPdt.method == "pdt" && cmpPdt.confidence == 1.0 &&
           std::abs(cmpPdt.offsetMs - 1500.0) < 1e-6 && cmpPdt.comparable);
    const auto cmpRel = compareTimelines(relative, absolute);
    expect("missing PDT -> relative, confidence 0",
           cmpRel.method == "relative" && cmpRel.confidence == 0.0 &&
           !cmpRel.comparable);
    const auto cmpManual = compareTimelines(relative, absolute, 800.0);
    expect("manual anchor -> medium confidence",
           cmpManual.method == "manual-anchor" &&
           cmpManual.confidence == 0.5 && cmpManual.comparable &&
           std::abs(cmpManual.offsetMs - 800.0) < 1e-6);
    const auto cmpEmpty = compareTimelines(AlignmentReport{}, absolute);
    expect("empty timeline -> unavailable", cmpEmpty.method == "unavailable");

    // ---- wire format round-trip --------------------------------------------
    ma::AnalysisResult wire = makeResult();
    wire.frames[0].segmentIndex = 0;
    wire.frames[0].wallClockMs = *absolute.firstWallClockMs;
    wire.alignment = std::make_shared<AlignmentReport>(absolute);
    Diagnostic d;
    d.code = Errc::Ok;
    const std::string json = ma::core::toJson(wire, d, true, "result", "test", 0,
                                              &cmpPdt);
    ma::AnalysisResult back;
    Diagnostic backDiag;
    TimelineComparison backCmp;
    const bool parsed = ma::core::fromJson(json, back, backDiag, &backCmp);
    expect("alignment round-trip parses", parsed && back.alignment != nullptr);
    expect("alignment round-trip timeline",
           back.alignment && back.alignment->timeline == TimelineKind::ProgramDateTime);
    expect("alignment round-trip playlist / issues / streams",
           back.alignment &&
           back.alignment->playlist.segments.size() == 3 &&
           back.alignment->issues.size() == absolute.issues.size() &&
           back.alignment->streams.size() == absolute.streams.size());
    expect("frame segment + wallClock round-trip",
           !back.frames.empty() && back.frames[0].segmentIndex == 0 &&
           back.frames[0].wallClockMs == wire.frames[0].wallClockMs);
    expect("comparison round-trip",
           backCmp.method == "pdt" &&
           std::abs(backCmp.offsetMs - 1500.0) < 1e-6);

    AnalysisOptions opts;
    opts.enableAlignment = false;
    AnalysisOptions optsBack;
    ma::core::fromOptionsJson(ma::core::toOptionsJson(opts), optsBack);
    expect("options round-trip enableAlignment", !optsBack.enableAlignment);

    std::printf("\n%d/%d checks passed\n", gTotal - gFailed, gTotal);
    return gFailed == 0 ? 0 : 1;
}
