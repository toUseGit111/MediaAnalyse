#pragma once
// ma::core - HLS timeline + A/V alignment diagnostics (pure C++20, no FFmpeg/Qt).
//
// Design contract (2026-11-09, from real HLS recording triage):
//   #EXT-X-PROGRAM-DATE-TIME is OPTIONAL. Today's recordings do not carry it,
//   future ones may. Therefore the timeline is a single model with three
//   automatically selected fidelity levels, NOT two implementations:
//
//     ProgramDateTime  : PDT present -> absolute wall clock, cross-file
//                        alignment is exact (e.g. two recorders of one event)
//     PlaylistElapsed  : no PDT, but EXTINF present -> elapsed time from
//                        sum(EXTINF); in-file discontinuity / missing segment /
//                        A/V skew still fully diagnosable
//     PtsOnly          : no readable playlist (single .ts / opaque URL) ->
//                        stream-internal + inter-stream diagnostics only
//
//   Adding PDT tags to an existing playlist upgrades the SAME code path to
//   absolute time: no flag, no rebuild, no schema change. Everything that
//   cannot be proven without a wall clock is reported as an explicit
//   `timeline-relative` info issue instead of being silently guessed.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"

namespace ma {

// ---------------------------------------------------------------- playlist ---

struct HlsSegment {
    int index = -1;                    // 0-based order inside the playlist
    int64_t mediaSequence = -1;        // EXT-X-MEDIA-SEQUENCE + index
    std::string uri;
    double extinfSeconds = 0.0;        // #EXTINF
    std::optional<int64_t> pdtMs;      // epoch ms, only if tag exists
    bool discontinuity = false;        // preceded by #EXT-X-DISCONTINUITY
    double playlistStartSeconds = 0.0; // sum of preceding EXTINF values

    // Filled by the demuxer while reading (see ProbeAnalyzer's io_open hook).
    bool opened = false;
    int packetCount = 0;
    std::optional<double> firstPtsSeconds;
    std::optional<double> lastPtsSeconds;
    // First pts of each stream inside this segment. Needed to measure A/V drift
    // separately: a mixed minimum would make both streams show the same slope.
    std::map<int, double> firstPtsByStream;
};

struct HlsPlaylist {
    std::string source;                 // url/path the text came from
    std::vector<HlsSegment> segments;
    double targetDuration = 0.0;
    int64_t mediaSequence = 0;
    bool hasEndList = false;
    bool isMaster = false;              // #EXT-X-STREAM-INF (variant list)
    int pdtCount = 0;                   // segments carrying PDT

    bool empty() const { return segments.empty(); }
    bool hasPdt() const { return pdtCount > 0; }
    bool allPdt() const {
        return !segments.empty() && pdtCount == static_cast<int>(segments.size());
    }
    double totalDuration() const {
        return segments.empty() ? 0.0
                                : segments.back().playlistStartSeconds +
                                      segments.back().extinfSeconds;
    }
};

// Parse a media playlist. Tolerant of CRLF, comments, BYTERANGE/KEY tags and
// missing EXTINF. Master playlists are flagged (isMaster) but still parsed so
// the caller can report "variant list, not a media playlist".
HlsPlaylist parseHlsPlaylist(const std::string& text, const std::string& source = {});

// ISO-8601 -> epoch milliseconds. Accepts "Z", "+0800", "+08:00", fractional
// seconds. Returns nullopt on malformed input. Uses a proleptic-Gregorian day
// count so it behaves identically on every platform (no timegm/_mkgmtime).
std::optional<int64_t> parseIso8601ToEpochMs(const std::string& s);

// Format epoch ms back to "YYYY-MM-DDThh:mm:ss.mmmZ" (UTC). Empty on invalid.
std::string formatEpochMsUtc(int64_t ms);

// --------------------------------------------------------------- diagnostics --

enum class TimelineKind { PtsOnly, PlaylistElapsed, ProgramDateTime };

inline const char* toString(TimelineKind k) {
    switch (k) {
    case TimelineKind::PtsOnly: return "pts-only";
    case TimelineKind::PlaylistElapsed: return "playlist-elapsed";
    case TimelineKind::ProgramDateTime: return "program-date-time";
    }
    return "pts-only";
}

inline TimelineKind timelineKindFromString(const std::string& s) {
    if (s == "program-date-time") return TimelineKind::ProgramDateTime;
    if (s == "playlist-elapsed") return TimelineKind::PlaylistElapsed;
    return TimelineKind::PtsOnly;
}

struct AlignmentIssue {
    std::string severity;   // "info" | "warning" | "error"
    std::string code;       // stable machine-readable id, e.g. "segment-missing"
    std::string message;    // human-readable, Chinese in this build
    int streamIndex = -1;   // -1 = container level
    int segmentIndex = -1;  // -1 = not segment scoped
    double atSeconds = -1.0;
};

struct StreamSyncMeta {
    int streamIndex = -1;
    StreamKind kind = StreamKind::Unknown;
    std::string codec;
    int packetCount = 0;
    double firstPtsSeconds = 0.0;      // relative to the run's earliest pts
    double lastPtsSeconds = 0.0;
    double startOffsetMs = 0.0;        // vs the reference (first video) stream
    double driftMsPerMin = 0.0;        // Theil-Sen slope of pts vs playlist time
    int nonMonotonicCount = 0;
    double maxGapSeconds = 0.0;
    int discontinuityCount = 0;
};

struct AlignmentReport {
    TimelineKind timeline = TimelineKind::PtsOnly;
    std::string timelineNote;
    int referenceStream = -1;          // stream startOffsetMs is measured against it
    bool crossFileComparable = false;  // true only with a wall-clock anchor
    std::optional<int64_t> firstWallClockMs;
    std::optional<int64_t> lastWallClockMs;
    double coveredSeconds = 0.0;
    std::vector<StreamSyncMeta> streams;
    std::vector<AlignmentIssue> issues;
    HlsPlaylist playlist;              // includes demux-measured fields
    int warningCount = 0;
    int errorCount = 0;
};

// Pure post-processing over an already demuxed AnalysisResult. `playlist` may
// be empty (non-HLS input): the report then degrades to PtsOnly and still
// checks A/V start offset, pts monotonicity and gaps.
AlignmentReport computeAlignment(const AnalysisResult& r, const HlsPlaylist& playlist);

struct OffsetWindow {
    double startSeconds = 0.0;   // relative to the recording's own timeline
    double endSeconds = 0.0;
    double offsetMs = 0.0;       // median A-relative-to-B offset in this window
    int sampleCount = 0;
};

struct SyncAnomaly {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double offsetMs = 0.0;       // median offset inside the period
    double deviationMs = 0.0;    // vs the whole-run offset
    std::string kind;            // "local-shift" | "step" | "drift"
};

struct TimelineComparison {
    bool comparable = false;
    // "pdt" | "content-signature" | "segment-pattern" | "manual-anchor" |
    // "relative" | "unavailable"
    std::string method;
    double offsetMs = 0.0;   // positive = A starts later than B
    double confidence = 0.0; // 1.0 = wall clock, 0.5 = manual anchor, 0 = guess
    double overlapSeconds = 0.0;
    std::string note;
    std::optional<int64_t> firstWallClockA;
    std::optional<int64_t> firstWallClockB;

    // Time-varying diagnostics (content-signature method only). When a
    // recording drifts or jumps out of sync in the middle, the single
    // `offsetMs` above is not enough: `windows` is the offset curve,
    // `anomalies` lists the concrete periods that deviate from it.
    double windowSeconds = 0.0;
    double driftMsPerMin = 0.0;
    double maxDeviationMs = 0.0;
    std::vector<OffsetWindow> windows;
    std::vector<SyncAnomaly> anomalies;
};

// Best-effort offset recovery without a wall-clock tag. When two recordings
// carry the same encoded elementary stream, their keyframes have identical
// packet sizes and picture types. Matching those signatures yields a constant
// PTS offset even when neither playlist has #EXT-X-PROGRAM-DATE-TIME. This is
// deliberately reported as medium confidence: it proves the frames match, not
// that the two recorders shared a clock.
struct ContentOffset {
    bool found = false;
    double offsetMs = 0.0;       // A relative to B, whole-run median
    double confidence = 0.0;     // 0..0.85, scales with the number of matches
    int matchedPairs = 0;        // votes in the winning cluster
    int signatureCount = 0;      // smaller side's keyframe signatures
    std::string note;
    double windowSeconds = 10.0;
    double driftMsPerMin = 0.0;
    double maxDeviationMs = 0.0;
    std::vector<OffsetWindow> windows;
    std::vector<SyncAnomaly> anomalies;
};
ContentOffset estimateContentOffset(const AnalysisResult& a,
                                    const AnalysisResult& b);

// Fallback for two recordings of the same live event encoded differently:
// their TS segments still share packet-count / EXTINF patterns. Matching the
// segment sequence gives a relative offset curve (the absolute shift can be
// ambiguous when the GOP pattern repeats, so confidence stays low).
ContentOffset estimateSegmentPatternOffset(const AlignmentReport& a,
                                           const AlignmentReport& b);

// Compare two recordings. With PDT on both sides this is an exact wall-clock
// offset. Without PDT, matching frame signatures are tried first; an operator
// may still override everything with `manualAnchorMs` (a known sync point such
// as a clap / timecode). Passing the full results enables the signature path;
// the CLI and the GUI worker both keep their frame lists, so they do.
TimelineComparison compareTimelines(const AlignmentReport& a,
                                    const AlignmentReport& b,
                                    std::optional<double> manualAnchorMs = std::nullopt,
                                    const AnalysisResult* aResult = nullptr,
                                    const AnalysisResult* bResult = nullptr);

} // namespace ma
