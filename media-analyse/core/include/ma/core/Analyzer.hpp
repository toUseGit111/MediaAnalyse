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

    // Passed verbatim to the AVDictionary, e.g. rtsp_transport=tcp,
    // method=post, headers=..., reconnect=1, protocol_whitelist=...
    std::vector<std::pair<std::string, std::string>> extraOptions;
};

struct Progress {
    int64_t packetsRead = 0;
    double elapsedSeconds = 0.0;
};

using ProgressFn = std::function<void(const Progress&)>;
using CancelFlag = std::atomic<bool>;

class Analyzer {
public:
    virtual ~Analyzer() = default;
    // `cancel` may be null. Never throws; every failure is a Diagnostic.
    virtual Outcome<AnalysisResult> analyze(const std::string& rawInput,
                                            const AnalysisOptions& options,
                                            const CancelFlag* cancel,
                                            const ProgressFn& onProgress) = 0;
};

} // namespace ma
