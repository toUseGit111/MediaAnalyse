#pragma once
// ma::core - analyzer interface. Implementations live in ma::infra (FFmpeg).
#include <atomic>
#include <functional>

#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/UrlNormalizer.hpp"

namespace ma {

struct AnalysisOptions {
    // 0 = FFmpeg defaults. No hardcoded 10 MB / 20 s caps ever again.
    int64_t probesize = 0;          // bytes
    int64_t analyzeduration = 0;    // microseconds

    // Staged timeouts (seconds), replacing the legacy "6 s kill switch".
    double connectTimeout = 15.0;   // until avformat_open_input returns
    double stallTimeout = 15.0;     // no new data between packets
    double probeTimeout = 0.0;      // hard ceiling for find_stream_info;
                                    // 0 = auto max(60 s, 4 x stallTimeout)

    int64_t maxFrames = 200000;     // frames kept in memory (bounded store)
    // Decode to attach frame-level width/height/pix_fmt/sample_fmt/channels to
    // each packet row. It never overrides timing, pict_type or samples: with
    // B-frame reordering the Nth packet does not yield the Nth displayed frame.
    // Full decoding of a long 4K file is expensive and rarely needed for
    // troubleshooting, so only the first N decoded frames per stream are
    // collected. Exceeding the budget never discards the extras of earlier
    // rows: they stay visible, later rows simply remain packet-level.
    bool decodeFrames = false;
    int64_t maxDecodeFrames = 3000;   // per decodable (video/audio) stream
    bool keepUnknownStreams = true; // subtitle/data/attachment streams listed
    // HLS timeline / A/V alignment post-processing. Cheap (one pass over the
    // stored rows); disable with CLI --no-align when only raw packets matter.
    bool enableAlignment = true;

    // Passed verbatim to the AVDictionary, e.g. rtsp_transport=tcp,
    // method=post, headers=..., reconnect=1, protocol_whitelist=...
    std::vector<std::pair<std::string, std::string>> extraOptions;

    // Optional remote-source adapter (infra/CstoreFetcher). Empty means the
    // input is handed straight to FFmpeg as before. "cstore" treats `input` as
    // a cstore object key (e.g. seewo-edulyse/xxx.m3u8): the fetcher resolves
    // the playlist and every segment through the cstore downloadUrl API,
    // materializes them locally, and the existing analyzer reads the local
    // playlist. Segment numbering holes are preserved so the alignment pass
    // can report "segment-missing".
    std::string source;
    // Directory for the fetched playlist/TS files. Empty = auto under
    // <cwd>/fetched/<sanitized-key>/
    std::string fetchDir;
    int fetchJobs = 4;                // parallel segment downloads, 1..16
    double fetchTimeout = 30.0;       // seconds, per HTTP request

    // Optional second recording for cross-file alignment. Empty = single-file
    // analysis. When set, the caller analyzes BOTH inputs and compares their
    // timelines (see compareTimelines in Alignment.hpp). `anchorMs` is the
    // operator-supplied common event used only when the files carry no PDT;
    // negative = B starts later, positive = A starts later. It is emitted in
    // the result only as a low-confidence "manual-anchor" comparison.
    std::string compareInput;
    bool hasAnchorMs = false;
    double anchorMs = 0.0;
    // "cstore" makes the second input go through the same recording fetcher as
    // the primary source. Empty = the second input is a plain file or URL.
    std::string compareSource;
    // GUI dual-source mode: the worker publishes the second recording's full
    // frame list (tagged source:"B") instead of silently analyzing it only for
    // alignment. The CLI leaves this false, so its wire behaviour is unchanged.
    bool compareFullOutput = false;
};

struct Progress {
    int64_t packetsRead = 0;
    double elapsedSeconds = 0.0;
};

// Incremental view of an analysis that is still running. A live stream has no
// EOF, so waiting for the final AnalysisResult would leave the UI empty for as
// long as the user keeps watching it. The kernel publishes:
//   * one snapshot right after find_stream_info (hasStreams=true, header holds
//     the container + stream list, no frames),
//   * periodic snapshots with only the rows parsed since the previous one.
// The final result stays a complete, canonical AnalysisResult - snapshots are
// purely additive observation and never change the outcome semantics.
struct AnalysisSnapshot {
    bool hasStreams = false;
    AnalysisResult header;                 // container + streams; frames empty
    std::vector<FrameRecord> newFrames;    // rows since the last snapshot
    int64_t packetsRead = 0;
    double elapsedSeconds = 0.0;
};

using ProgressFn = std::function<void(const Progress&)>;
using SnapshotFn = std::function<void(const AnalysisSnapshot&)>;
using CancelFlag = std::atomic<bool>;

class Analyzer {
public:
    virtual ~Analyzer() = default;
    // `cancel` may be null. Never throws; every failure is a Diagnostic.
    // `onSnapshot` is optional (CLI passes nothing): when set, the kernel
    // streams header/row batches so a live source can be shown while reading.
    virtual Outcome<AnalysisResult> analyze(const std::string& rawInput,
                                            const AnalysisOptions& options,
                                            const CancelFlag* cancel,
                                            const ProgressFn& onProgress,
                                            const SnapshotFn& onSnapshot = {}) = 0;
};

} // namespace ma
