#pragma once
// ma::infra - cstore recording fetcher.
//
// Turns a cstore object key (the `xxx.m3u8` path used by the recording system)
// into a local playlist + TS files that the existing ProbeAnalyzer can read.
// It deliberately does not do any media diagnosis: after fetching, the caller
// hands the returned local m3u8 to the normal analyzer, so all existing HLS
// timeline / alignment checks (segment-missing, PDT gaps, A-V offset, ...)
// apply unchanged.
//
// External contract (verified against the live service):
//   POST http://cstore.seewo.com/cstore/api/v2/downloadUrl?isTmp=false&appId=10363
//        Content-Type: application/json
//        Referer: http://cstore.seewo.com
//        body: ["<object key>"]
//   resp: {"code":0,"data":[{"downloadUrl":"https://...","fileKey":"..."}]}
//
// A failed segment still occupies its slot in the generated local playlist:
// the URI line is written but the file is absent. FFmpeg skips the missing
// segment (confirmed on 9.0.2) and SegmentTracker leaves it `opened == false`,
// which the alignment pass reports as a single "segment-missing" issue.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ma/core/Analyzer.hpp"
#include "ma/core/Result.hpp"

namespace ma::infra {

struct FetchedSegment {
    int index = -1;                 // 0-based position in the source playlist
    std::string objectKey;          // key sent to the downloadUrl API
    std::string localName;          // segment_NNN.ts
    bool ok = false;
    int64_t bytes = 0;
    std::string error;
};

struct FetchReport {
    std::string localPlaylist;      // absolute path of the generated local.m3u8
    std::string directory;          // directory holding the playlist + TS files
    std::string objectKey;          // the m3u8 key we started from
    std::string playlistKey;        // resolved playlist key (master -> variant)
    bool isVariant = false;         // true when a master playlist was expanded
    int segmentsTotal = 0;          // TS lines found in the playlist
    int segmentsDownloaded = 0;
    int segmentsFailed = 0;
    std::vector<FetchedSegment> segments;
    double elapsedSeconds = 0.0;
};

using FetchProgressFn = std::function<void(int done, int total, const std::string& phase)>;

// Fetch the whole recording into `options.fetchDir` (or an auto directory).
// `cancel` may be null. Never throws: every failure is a Diagnostic.
Outcome<FetchReport> fetchCstoreRecording(const std::string& objectKey,
                                          const AnalysisOptions& options,
                                          const CancelFlag* cancel,
                                          const FetchProgressFn& onProgress = {});

// Convenience: one-line summary used in CLI/GUI notes, e.g.
// "cstore: 120 segments, 118 downloaded, 2 missing (005, 119)".
std::string describeFetch(const FetchReport& report);

} // namespace ma::infra
