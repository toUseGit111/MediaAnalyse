#pragma once
// ma::core - audio cross-correlation math. Pure C++20, no FFmpeg / Qt.
//
// Two cameras in one room see different pictures but share one sound field:
// a clap, a bell, a teacher's voice appears in both recordings. Correlating
// the two mono audio tracks therefore recovers the recording start offset
// independent of camera angle - the only reliable cross-view alignment when
// neither playlist carries #EXT-X-PROGRAM-DATE-TIME.
//
// This header is the MATH only (FFT + GCC-PHAT). Decoding media into the two
// float vectors is the job of ma::infra::decodeAudioMono, so unit tests can
// feed synthetic signals without linking FFmpeg.
#include <complex>
#include <optional>
#include <string>
#include <vector>

namespace ma {

// One window of the time-varying audio alignment track. The two recordings
// share one sound field but are NOT guaranteed to overlap completely (different
// start/stop times, a dropped section), so every window carries its own
// reliability instead of silently inheriting the global offset.
struct AudioOffsetWindow {
    double startSeconds = 0.0;   // relative to A's own timeline
    double endSeconds = 0.0;
    double offsetMs = 0.0;       // A relative to B; positive = A picture ahead
    double similarity = 0.0;     // raw normalised cross-correlation, 0..1
    double peakRatio = 0.0;
    bool reliable = false;
    std::string reason;          // "ok" | "silence" | "mismatch" | "too-short"
};

struct AudioTrackOptions {
    double windowSeconds = 8.0;      // analysis window length
    double hopSeconds = 8.0;         // window step (== length => no overlap)
    double localSearchSeconds = 4.0; // +/- jitter around the seed offset
    double minSimilarity = 0.25;     // NCC gate; below this the window is noise
};

struct AudioOffsetTrack {
    bool ok = false;
    double sampleRate = 0.0;
    double seedOffsetMs = 0.0;       // coarse global offset used as search centre
    double baseOffsetMs = 0.0;       // modal offset over reliable windows
    double driftMsPerMin = 0.0;
    double maxDeviationMs = 0.0;
    double reliableRatio = 0.0;      // reliable windows / total windows
    int reliableCount = 0;
    std::vector<AudioOffsetWindow> windows;
    std::string note;
};

struct AudioCorrelationResult {
    bool ok = false;
    // A relative to B, in ms. Positive = A started later than B (so a shared
    // sound appears EARLIER in A's local timeline). Same sign convention as
    // TimelineComparison. In terms of PICTURE progress, positive means A's
    // picture is AHEAD of B by offsetMs.
    double offsetMs = 0.0;
    // Peak sharpness (peak / rms of the correlation surface), ~2..20+.
    double peakRatio = 0.0;
    // Confidence 0..0.95 derived from peakRatio and lag unambiguousness.
    double confidence = 0.0;
    int sampleRate = 0;
    double analyzedSeconds = 0.0;
    // Full lag range scanned, seconds (e.g. 30 = +/-30 s).
    double maxLagSeconds = 30.0;
    std::string note;
    // Time-varying track filled by estimateAudioOffsetTrack (optional). When
    // present, offsetMs equals track.baseOffsetMs and the GUI can draw the
    // "who is ahead in which period" curve.
    bool hasTrack = false;
    AudioOffsetTrack track;
};

// Time-varying alignment: slide `windowSeconds` over both tracks (aligned
// through `seedOffsetMs`) and correlate each window locally. Only windows with
// a real waveform match are marked reliable, so a section present in only one
// recording, or silence, is reported instead of inventing an offset.
// Inputs are expected at ~4 kHz mono (infra::decodeAudioMono with
// targetRate=4000); any rate > 0 is accepted.
AudioOffsetTrack estimateAudioOffsetTrack(
    const std::vector<float>& a, const std::vector<float>& b,
    int sampleRate, double seedOffsetMs,
    const AudioTrackOptions& opt = {});

// Iterative radix-2 FFT (in place). `n` must be a power of two.
void fftInPlace(std::vector<std::complex<float>>& a, bool inverse);

// GCC-PHAT cross-correlation of two mono float tracks.
// `maxLagSeconds` bounds the search (default +/-30 s) so a long recording does
// not latch onto a periodic later occurrence. Returns the lag with the
// sharpest peak. Both inputs are used up to min(sizeA, sizeB, 180 s).
AudioCorrelationResult estimateAudioOffset(const std::vector<float>& a,
                                           const std::vector<float>& b,
                                           int sampleRate = 16000,
                                           double maxLagSeconds = 30.0);

}  // namespace ma
