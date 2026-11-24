#pragma once
// ma::infra - decode the audio track of a media file into 16 kHz mono float.
// Used by the dual-source audio cross-correlation alignment; lives in infra
// because it needs FFmpeg (the pure math lives in ma::core::AudioCorrelation).
#include <string>
#include <vector>

#include "ma/core/Result.hpp"
#include "ma/core/Analyzer.hpp"

namespace ma::infra {

// Decode up to `maxSeconds` of audio from `path`, converting to mono float at
// `targetRate` Hz (default 16000). 4000 Hz is enough for cross-correlation and
// keeps long recordings small, so the windowed alignment decodes at 4 kHz.
// Returns a Diagnostic on failure (no audio stream, decode error, cancelled);
// the sample vector is empty on failure.
ma::Outcome<std::vector<float>> decodeAudioMono(
    const std::string& path,
    const ma::CancelFlag* cancel,
    const ma::AnalysisOptions& options,
    double maxSeconds = 120.0,
    int targetRate = 16000);

// Backwards-compatible 16 kHz helper used by the original global-only path.
inline ma::Outcome<std::vector<float>> decodeAudioMono16k(
    const std::string& path, const ma::CancelFlag* cancel,
    const ma::AnalysisOptions& options, double maxSeconds = 120.0) {
    return decodeAudioMono(path, cancel, options, maxSeconds, 16000);
}

}  // namespace ma::infra
