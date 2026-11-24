// Pure-core audio GCC-PHAT tests: no FFmpeg, no media files.
#include "ma/core/AudioCorrelation.hpp"
#include "ma/core/Alignment.hpp"
#include "ma/core/Serialization.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

// Always 4 s long so both tracks fully contain their event within the
// correlation window regardless of where the event sits.
std::vector<float> tone(int sr, double startSec, double seconds, double freq) {
    std::vector<float> v(static_cast<size_t>(4.0 * sr), 0.0f);
    const size_t from = static_cast<size_t>(startSec * sr);
    const size_t to = std::min(v.size(),
                               static_cast<size_t>((startSec + seconds) * sr));
    for (size_t i = from; i < to; ++i)
        v[i] = static_cast<float>(0.6 * std::sin(2 * 3.14159265358979 * freq *
                                                i / sr));
    return v;
}

// Speech-like signal: a burst of sound in every 1 s slot, distinct frequency
// per slot so a windowed correlation can locate itself in time unambiguously.
std::vector<float> speechLike(int sr, int seconds) {
    // Deterministic pseudo-random noise with a per-second envelope: unique in
    // time (no periodic ambiguity) while still having silent-ish gaps.
    std::vector<float> v(static_cast<size_t>(seconds * sr), 0.0f);
    unsigned seed = 12345u;
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        seed = seed * 1103515245u + 12345u;
        const double r = static_cast<double>((seed >> 8) & 0xffff) / 32768.0 - 1.0;
        const int slot = i / sr;
        const double env = 0.3 + 0.7 * std::fabs(
            std::sin(3.14159265358979 * (i % sr) / sr));
        (void)slot;
        v[i] = static_cast<float>(0.5 * env * r);
    }
    return v;
}

// Shift `src` right by `delayMs`; the returned buffer is longer so no content
// is lost, matching a recording that started `delayMs` earlier.
std::vector<float> delay(const std::vector<float>& src, int sr, double delayMs) {
    const size_t d = static_cast<size_t>(delayMs / 1000.0 * sr);
    std::vector<float> out(d + src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); ++i) out[i + d] = src[i];
    return out;
}

}  // namespace

int main() {
    const int sr = 16000;
    // A has a clap-like tone at t=1.0 s. B is the same tone at t=1.35 s,
    // i.e. B started 350 ms earlier -> A started 350 ms later.
    auto a = tone(sr, 1.0, 0.25, 440.0);
    auto b = tone(sr, 1.35, 0.25, 440.0);

    const auto r = ma::estimateAudioOffset(a, b, sr, 5.0);
    expect(r.ok, "GCC-PHAT finds a peak");
    expect(std::abs(r.offsetMs - 350.0) < 5.0, "offset is +350 ms (A later)");
    expect(r.peakRatio > 3.0, "peak is sharp");
    expect(r.confidence > 0.5, "high confidence for a clean shared event");

    const auto swapped = ma::estimateAudioOffset(b, a, sr, 5.0);
    expect(swapped.ok && std::abs(swapped.offsetMs + 350.0) < 5.0,
           "swapped inputs flip the sign to -350 ms");

    // No common audio -> must not invent a precise offset.
    auto noise = a;
    for (size_t i = 0; i < noise.size(); ++i)
        noise[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 *
                                                    137.0 * i / sr));
    const auto unrelated = ma::estimateAudioOffset(a, noise, sr, 5.0);
    expect(!unrelated.ok || unrelated.confidence < 0.5,
           "unrelated audio does not claim high confidence");

    // compareTimelines honours the audio result over a manual anchor.
    ma::AlignmentReport ra, rb;
    ra.streams.resize(1);
    rb.streams.resize(1);
    ra.coveredSeconds = rb.coveredSeconds = 60.0;
    const auto cmp = ma::compareTimelines(ra, rb, /*manualAnchorMs=*/0.0,
                                          nullptr, nullptr, &r);
    expect(cmp.method == "audio-xcorr", "audio-xcorr wins over manual anchor");
    expect(std::abs(cmp.offsetMs - 350.0) < 5.0, "comparison keeps audio offset");
    expect(std::abs(cmp.audioPeakRatio - r.peakRatio) < 1e-6,
           "peak ratio survives into the comparison");

    // Serialization round-trip of the new fields.
    auto json = ma::core::toJson(ma::AnalysisResult{}, ma::Diagnostic{},
                                 false, "result", "", 0, &cmp);
    ma::AnalysisResult back;
    ma::Diagnostic backDiag;
    ma::TimelineComparison backCmp;
    expect(ma::core::fromJson(json, back, backDiag, &backCmp),
           "comparison JSON parses");
    expect(backCmp.method == "audio-xcorr" &&
               std::abs(backCmp.offsetMs - 350.0) < 5.0 &&
               std::abs(backCmp.audioPeakRatio - r.peakRatio) < 1e-6,
           "audio comparison fields round-trip");

    // Option flags round-trip too.
    ma::AnalysisOptions o;
    o.audioAlign = true;
    o.audioMaxSeconds = 90.0;
    o.audioMaxLagSeconds = 20.0;
    const auto oj = ma::core::toOptionsJson(o);
    ma::AnalysisOptions bo;
    expect(ma::core::fromOptionsJson(oj, bo) && bo.audioAlign &&
               std::abs(bo.audioMaxSeconds - 90.0) < 1e-6 &&
               std::abs(bo.audioMaxLagSeconds - 20.0) < 1e-6,
           "audio options round-trip");

    // ---- Time-varying (windowed) audio alignment --------------------------
    const int tr = 4000;  // windowed path decimates to 4 kHz
    const int secs = 24;
    auto speech = speechLike(tr, secs);

    // Case 1: constant +350 ms offset across the whole recording.
    auto delayed = delay(speech, tr, 350.0);
    auto seed = ma::estimateAudioOffset(speech, delayed, tr, 5.0);
    expect(seed.ok && std::abs(seed.offsetMs - 350.0) < 8.0,
           "windowed: global seed recovers +350 ms");
    ma::AudioTrackOptions opt;
    opt.windowSeconds = 4.0;
    opt.hopSeconds = 4.0;
    opt.localSearchSeconds = 1.5;
    auto track = ma::estimateAudioOffsetTrack(speech, delayed, tr,
                                              seed.offsetMs, opt);
    expect(track.ok, "windowed: track is produced");
    expect(track.reliableCount >= 3, "windowed: several reliable windows");
    expect(std::abs(track.baseOffsetMs - 350.0) < 10.0,
           "windowed: modal offset is +350 ms");
    expect(track.windows.size() >= 4, "windowed: covers the whole timeline");
    bool allMatch = true;
    for (const auto& w : track.windows)
        if (w.reliable && std::abs(w.offsetMs - 350.0) > 15.0) allMatch = false;
    expect(allMatch, "windowed: every reliable window agrees with +350 ms");

    // Case 2: a mid-recording step. B is a delayed copy of A by 350 ms for the
    // first half, then by 1350 ms (B falls 1 s further behind) for the second
    // half. The track must show both states, not a single averaged offset.
    const size_t split = static_cast<size_t>(12 * tr);
    std::vector<float> stepped(speech.size() + static_cast<size_t>(1.35 * tr),
                               0.0f);
    for (size_t i = 0; i < speech.size(); ++i) {
        const size_t d = i < split ? static_cast<size_t>(0.35 * tr)
                                   : static_cast<size_t>(1.35 * tr);
        stepped[i + d] = speech[i];
    }
    auto seed2 = ma::estimateAudioOffset(speech, stepped, tr, 5.0);
    auto track2 = ma::estimateAudioOffsetTrack(speech, stepped, tr,
                                               seed2.ok ? seed2.offsetMs : 350.0,
                                               opt);
    expect(track2.ok && track2.reliableCount >= 3,
           "windowed(step): track produced across the step");
    // Reliable windows should cluster near 350 and 1350, never average to 850.
    bool hasEarly = false, hasLate = false;
    for (const auto& w : track2.windows) {
        if (!w.reliable) continue;
        if (std::abs(w.offsetMs - 350.0) < 60.0) hasEarly = true;
        if (std::abs(w.offsetMs - 1350.0) < 60.0) hasLate = true;
    }
    expect(hasEarly && hasLate,
           "windowed(step): both 350 ms and 1350 ms states are recovered");

    // Case 3: one recording is silent for a stretch (not fully overlapping).
    auto partlySilent = delayed;
    for (size_t i = static_cast<size_t>(8 * tr);
         i < static_cast<size_t>(12 * tr) && i < partlySilent.size(); ++i)
        partlySilent[i] = 0.0f;
    auto track3 = ma::estimateAudioOffsetTrack(speech, partlySilent, tr,
                                               seed.offsetMs, opt);
    expect(track3.ok, "windowed(overlap): track survives a silent stretch");
    int rejected = 0;
    for (const auto& w : track3.windows)
        if (!w.reliable && w.reason == "mismatch") ++rejected;
    expect(rejected >= 1,
           "windowed(overlap): non-overlapping windows are rejected, not guessed");

    if (failures) {
        std::fprintf(stderr, "%d audio tests failed\n", failures);
        return 1;
    }
    std::printf("all audio correlation tests passed\n");
    return 0;
}
