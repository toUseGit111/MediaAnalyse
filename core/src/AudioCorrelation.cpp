#include "ma/core/AudioCorrelation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ma {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Averaging decimator with a cheap box anti-alias filter. Speech and clap
// transients live well below 4 kHz, so 4 kHz is plenty and keeps the FFT small.
std::vector<float> decimateTo4k(const std::vector<float>& in, int sampleRate,
                                int& outRate) {
    const int factor = std::max(1, sampleRate / 4000);
    outRate = sampleRate / factor;
    if (factor == 1) return in;  // already at (or below) 4 kHz
    std::vector<float> out;
    out.reserve(in.size() / static_cast<size_t>(factor));
    for (size_t i = 0; i + static_cast<size_t>(factor) <= in.size();
         i += static_cast<size_t>(factor)) {
        float sum = 0.0f;
        for (int j = 0; j < factor; ++j) sum += in[i + static_cast<size_t>(j)];
        out.push_back(sum / static_cast<float>(factor));
    }
    return out;
}

double magnitude(std::complex<float> v) {
    return std::sqrt(static_cast<double>(v.real()) * v.real() +
                     static_cast<double>(v.imag()) * v.imag());
}

// Result of aligning a short window `a` inside a longer segment `b`.
struct WindowCorr {
    bool ok = false;
    double lagSamples = 0.0;   // b offset at which a aligns, [0, bLen-aLen]
    double ncc = 0.0;          // raw normalised cross-correlation at the peak
    double peakRatio = 0.0;    // PHAT peak / rms of the search surface
};

// GCC-PHAT of a short window against a longer search segment. Unlike
// estimateAudioOffset this does not assume a symmetric +/- lag range: it
// scans every legal lag in [0, bLen-aLen], which is what a windowed search
// around a known seed needs.
WindowCorr correlateWindow(const std::vector<float>& a,
                           const std::vector<float>& b) {
    WindowCorr out;
    const size_t aLen = a.size(), bLen = b.size();
    if (aLen < 64 || bLen <= aLen) return out;
    const size_t maxLag = bLen - aLen;

    size_t n = 1;
    while (n < aLen + bLen) n <<= 1;
    std::vector<std::complex<float>> fa(n), fb(n);
    double ea = 0.0, eb = 0.0;
    for (size_t i = 0; i < aLen; ++i) {
        fa[i] = std::complex<float>(a[i], 0.0f);
        ea += static_cast<double>(a[i]) * a[i];
    }
    for (size_t i = 0; i < bLen; ++i) {
        fb[i] = std::complex<float>(b[i], 0.0f);
        eb += static_cast<double>(b[i]) * b[i];
    }
    if (ea <= 0.0 || eb <= 0.0) return out;
    fftInPlace(fa, false);
    fftInPlace(fb, false);

    std::vector<std::complex<float>> phat(n), raw(n);
    for (size_t i = 0; i < n; ++i) {
        const std::complex<float> prod = std::conj(fa[i]) * fb[i];
        const double mag = magnitude(prod);
        phat[i] = mag > 1e-9f ? prod / static_cast<float>(mag)
                              : std::complex<float>(0.0f, 0.0f);
        raw[i] = prod;
    }
    fftInPlace(phat, true);
    fftInPlace(raw, true);

    const double norm = std::sqrt(ea * eb);
    size_t best = 0;
    double bestMag = -1.0, sumSq = 0.0;
    for (size_t lag = 0; lag <= maxLag; ++lag) {
        const double m = magnitude(phat[lag]);
        sumSq += m * m;
        if (m > bestMag) { bestMag = m; best = lag; }
    }
    if (bestMag <= 0.0) return out;

    double refined = static_cast<double>(best);
    const auto magAt = [&](long lag) -> double {
        if (lag < 0 || static_cast<size_t>(lag) >= n) return 0.0;
        return magnitude(phat[static_cast<size_t>(lag)]);
    };
    if (best > 0 && best < maxLag) {
        const double y0 = magAt(static_cast<long>(best) - 1);
        const double y1 = bestMag;
        const double y2 = magAt(static_cast<long>(best) + 1);
        const double denom = y0 - 2.0 * y1 + y2;
        if (std::fabs(denom) > 1e-12) {
            const double delta = 0.5 * (y0 - y2) / denom;
            if (delta > -1.0 && delta < 1.0) refined += delta;
        }
    }
    const size_t rmsCount = maxLag + 1;
    const double rms = std::sqrt(sumSq / static_cast<double>(rmsCount));
    out.peakRatio = rms > 0.0 ? bestMag / rms : 0.0;
    out.ncc = magnitude(raw[best]) / norm;
    out.lagSamples = refined;
    out.ok = true;
    return out;
}

double signalRms(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    return std::sqrt(sum / static_cast<double>(v.size()));
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Modal offset: largest cluster of values within `tol` of each other, then the
// cluster median. With a mid-recording step the plain median lands between the
// two real states and makes every window look wrong; the mode keeps the
// dominant state as the reference.
double modalOffset(const std::vector<double>& values, double tol) {
    if (values.empty()) return 0.0;
    double best = values.front();
    size_t bestCount = 0;
    for (double c : values) {
        std::vector<double> near;
        for (double x : values)
            if (std::fabs(x - c) <= tol) near.push_back(x);
        if (near.size() > bestCount) {
            bestCount = near.size();
            best = median(near);
        }
    }
    return best;
}

}  // namespace

void fftInPlace(std::vector<std::complex<float>>& a, bool inverse) {
    const size_t n = a.size();
    if (n < 2) return;

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    // Cooley-Tukey iterative radix-2.
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) *
                           (inverse ? 1.0 : -1.0);
        const std::complex<float> wlen(
            static_cast<float>(std::cos(ang)),
            static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            const size_t half = len >> 1;
            for (size_t j = 0; j < half; ++j) {
                const std::complex<float> u = a[i + j];
                const std::complex<float> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const float inv = 1.0f / static_cast<float>(n);
        for (auto& v : a) v *= inv;
    }
}

AudioCorrelationResult estimateAudioOffset(const std::vector<float>& a,
                                           const std::vector<float>& b,
                                           int sampleRate,
                                           double maxLagSeconds) {
    AudioCorrelationResult out;
    out.sampleRate = sampleRate;
    out.maxLagSeconds = maxLagSeconds;
    if (sampleRate <= 0 || a.empty() || b.empty()) {
        out.note = "音频样本为空";
        return out;
    }

    int rate = 0;
    const std::vector<float> fa = decimateTo4k(a, sampleRate, rate);
    const std::vector<float> fb = decimateTo4k(b, sampleRate, rate);
    const size_t len = std::min({fa.size(), fb.size(),
                                 static_cast<size_t>(120.0 * rate)});
    if (len < static_cast<size_t>(rate)) {  // need at least 1 s
        out.note = "音频太短，无法做互相关（至少需要 1 秒重叠）";
        return out;
    }
    out.sampleRate = rate;
    out.analyzedSeconds = static_cast<double>(len) / rate;

    // Circular correlation needs 2x padding to avoid wrap-around.
    size_t n = 1;
    while (n < len * 2) n <<= 1;
    std::vector<std::complex<float>> faF(n), fbF(n);
    for (size_t i = 0; i < len; ++i) {
        faF[i] = std::complex<float>(fa[i], 0.0f);
        fbF[i] = std::complex<float>(fb[i], 0.0f);
    }
    fftInPlace(faF, false);
    fftInPlace(fbF, false);

    // GCC-PHAT whiten: conj(A)*B / |conj(A)*B|. Suppresses room reverb and
    // differing microphone frequency responses; keeps only phase (time) cues.
    // Keep the RAW cross spectrum too: PHAT alone can produce a sharp peak for
    // two unrelated periodic tones (same phase, different frequency). The
    // raw normalised cross-correlation tells us whether the waveforms really
    // match, which is the guard against claiming a false precise offset.
    std::vector<std::complex<float>> spec(n), rawSpec(n);
    double faEnergy = 0.0, fbEnergy = 0.0;
    for (size_t i = 0; i < len; ++i) {
        faEnergy += fa[i] * fa[i];
        fbEnergy += fb[i] * fb[i];
    }
    for (size_t i = 0; i < n; ++i) {
        const std::complex<float> prod = std::conj(faF[i]) * fbF[i];
        const double mag = magnitude(prod);
        spec[i] = mag > 1e-9f
                      ? prod / static_cast<float>(mag)
                      : std::complex<float>(0.0f, 0.0f);
        rawSpec[i] = prod;
    }
    faF.clear();
    fbF.clear();
    fftInPlace(spec, true);
    fftInPlace(rawSpec, true);
    const double norm = std::sqrt(faEnergy * fbEnergy);
    const auto rawAt = [&](long lag) {
        if (norm <= 0.0) return 0.0;
        const size_t idx = lag < 0 ? n + static_cast<size_t>(lag)
                                   : static_cast<size_t>(lag);
        return magnitude(rawSpec[idx]) / norm;
    };

    // Peak of the whitened correlation = the lag at which B's waveform
    // matches A's. A positive lag means the shared event appears later in B's
    // timeline, i.e. A started later - matching TimelineComparison's sign.
    const long maxLag = static_cast<long>(std::min(
        maxLagSeconds * rate, static_cast<double>(len) - 1.0));
    long best = 0;
    double bestMag = -1.0;
    double sumSq = 0.0;
    int counted = 0;
    for (long lag = -maxLag; lag <= maxLag; ++lag) {
        const size_t idx = lag < 0 ? n + static_cast<size_t>(lag)
                                   : static_cast<size_t>(lag);
        const double m = magnitude(spec[idx]);
        sumSq += m * m;
        ++counted;
        if (m > bestMag) {
            bestMag = m;
            best = lag;
        }
    }
    if (bestMag <= 0.0 || counted == 0) {
        out.note = "互相关没有有效峰值";
        return out;
    }

    const double rms = std::sqrt(sumSq / counted);
    out.peakRatio = rms > 0.0 ? bestMag / rms : 0.0;

    // Normalised cross-correlation at the chosen lag: 1.0 means the two
    // waveforms are genuinely the same sound; near 0 means PHAT found only a
    // phase coincidence (typical for two different steady tones).
    const double ncc = rawAt(best);
    if (ncc < 0.18) {
        out.note = "两路音频波形不相似（归一化互相关仅 " +
                   std::to_string(static_cast<int>(ncc * 100)) +
                   "%），互相关峰值是相位巧合，结果不可用";
        return out;
    }

    // Sub-sample parabolic interpolation on the peak for sub-ms accuracy.
    double refined = static_cast<double>(best);
    const auto at = [&](long lag) {
        const size_t idx = lag < 0 ? n + static_cast<size_t>(lag)
                                   : static_cast<size_t>(lag);
        return magnitude(spec[idx]);
    };
    if (best > -maxLag && best < maxLag) {
        const double y0 = at(best - 1), y1 = bestMag, y2 = at(best + 1);
        const double denom = y0 - 2.0 * y1 + y2;
        if (std::fabs(denom) > 1e-12) {
            const double delta = 0.5 * (y0 - y2) / denom;
            if (delta > -1.0 && delta < 1.0) refined += delta;
        }
    }

    out.offsetMs = refined * 1000.0 / rate;

    // Ambiguity check: a second peak near the main one within 15% of its
    // height means the two recordings may have repetitive audio (e.g. a
    // metronome) and the chosen lag is not unique.
    double second = 0.0;
    const long guard = std::max<long>(2, static_cast<long>(0.05 * rate));
    for (long lag = -maxLag; lag <= maxLag; ++lag) {
        if (std::labs(lag - best) <= guard) continue;
        second = std::max(second, at(lag));
    }
    const double ambiguity = bestMag > 0.0 ? second / bestMag : 1.0;

    double conf = (out.peakRatio - 1.8) / 8.0;
    conf *= std::clamp((ncc - 0.15) / 0.5, 0.0, 1.0);
    conf *= (1.0 - 0.6 * std::clamp(ambiguity, 0.0, 1.0));
    out.confidence = std::clamp(conf, 0.0, 0.95);
    if (out.peakRatio < 2.0) {
        out.confidence = 0.0;
        out.note = "音频相关性太弱（可能是静音或两路声音完全不同），结果不可用";
        return out;
    }
    out.ok = true;
    out.note = "两路音频 GCC-PHAT 互相关峰值 " +
               std::to_string(static_cast<int>(out.peakRatio * 10) / 10.0) +
               " 倍、波形相似度 " +
               std::to_string(static_cast<int>(ncc * 100)) +
               "%，A 相对 B " +
               std::to_string(static_cast<int>(out.offsetMs * 10) / 10.0) +
               " ms；基于共同声场事件，与摄像头视角无关" +
               (ambiguity > 0.55 ? "（存在周期性歧义，置信度已下调）" : "");
    return out;
}

AudioOffsetTrack estimateAudioOffsetTrack(const std::vector<float>& a,
                                          const std::vector<float>& b,
                                          int sampleRate,
                                          double seedOffsetMs,
                                          const AudioTrackOptions& opt) {
    AudioOffsetTrack out;
    out.sampleRate = sampleRate;
    out.seedOffsetMs = seedOffsetMs;
    if (sampleRate <= 0 || a.empty() || b.empty()) {
        out.note = "音频样本为空，无法做分时段对齐";
        return out;
    }

    int rate = 0;
    const std::vector<float> fa = decimateTo4k(a, sampleRate, rate);
    const std::vector<float> fb = decimateTo4k(b, sampleRate, rate);
    out.sampleRate = rate;

    const double windowSec = std::clamp(opt.windowSeconds, 1.0, 60.0);
    const double hopSec = std::clamp(opt.hopSeconds, 1.0, 60.0);
    const double searchSec = std::clamp(opt.localSearchSeconds, 0.25, 30.0);
    const double minSim = std::clamp(opt.minSimilarity, 0.05, 0.9);
    const size_t W = static_cast<size_t>(windowSec * rate);
    const size_t H = static_cast<size_t>(hopSec * rate);
    const size_t S = static_cast<size_t>(searchSec * rate);
    const size_t usable = std::min(fa.size(), fb.size());
    if (usable < W + 2 * S) {
        out.note = "音频太短，无法做分时段互相关";
        return out;
    }

    const double rmsA = signalRms(fa);
    const long long seedSamples =
        static_cast<long long>(llround(seedOffsetMs / 1000.0 * rate));

    // Walk A's timeline; the shared sound is expected near (tA + seed).
    for (size_t a0 = 0; a0 + W <= fa.size(); a0 += H) {
        AudioOffsetWindow w;
        w.startSeconds = static_cast<double>(a0) / rate;
        w.endSeconds = static_cast<double>(a0 + W) / rate;

        const std::vector<float> aw(fa.begin() + static_cast<long>(a0),
                                    fa.begin() + static_cast<long>(a0 + W));
        // A window that is silence carries no evidence either way.
        if (rmsA <= 0.0 || signalRms(aw) < rmsA * 0.08) {
            w.reason = "silence";
            out.windows.push_back(std::move(w));
            continue;
        }

        long long b0 = static_cast<long long>(a0) + seedSamples -
                       static_cast<long long>(S);
        b0 = std::max<long long>(0, b0);
        const size_t need = W + 2 * S;
        if (b0 + static_cast<long long>(need) >
            static_cast<long long>(fb.size())) {
            // Tail of the recording: shrink the search rather than dropping
            // the window, so the last seconds still get an offset estimate.
            b0 = static_cast<long long>(fb.size()) -
                 static_cast<long long>(need);
            b0 = std::max<long long>(0, b0);
        }
        const std::vector<float> bseg(
            fb.begin() + static_cast<long>(b0),
            fb.begin() + static_cast<long>(b0 + need));

        const WindowCorr wc = correlateWindow(aw, bseg);
        if (!wc.ok) {
            w.reason = "too-short";
            out.windows.push_back(std::move(w));
            continue;
        }
        w.similarity = std::clamp(wc.ncc, 0.0, 1.0);
        w.peakRatio = wc.peakRatio;
        // The window at a0 in A maps to (a0 + seed) in B. Subtract the
        // search segment start b0 to get the expected lag inside bseg.
        const double expectedLag = static_cast<double>(a0) +
                                   static_cast<double>(seedSamples) -
                                   static_cast<double>(b0);
        const double lagMs = (wc.lagSamples - expectedLag) * 1000.0 / rate;
        w.offsetMs = seedOffsetMs + lagMs;

        const bool withinSearch = std::fabs(lagMs) <=
                                  (searchSec * 1000.0 + 50.0);
        if (w.similarity < minSim || w.peakRatio < 1.6 || !withinSearch) {
            w.reason = "mismatch";
            out.windows.push_back(std::move(w));
            continue;
        }
        w.reliable = true;
        w.reason = "ok";
        out.windows.push_back(std::move(w));
    }

    if (out.windows.empty()) {
        out.note = "没有可分析的时间窗";
        return out;
    }

    std::vector<double> reliableOffsets;
    for (const auto& w : out.windows)
        if (w.reliable) reliableOffsets.push_back(w.offsetMs);
    out.reliableCount = static_cast<int>(reliableOffsets.size());
    out.reliableRatio = static_cast<double>(out.reliableCount) /
                        static_cast<double>(out.windows.size());
    out.ok = !reliableOffsets.empty();
    if (!out.ok) {
        out.note = "所有时间窗的音频都不匹配（可能只有一路有声音，或两路声场不同）";
        return out;
    }

    out.baseOffsetMs = modalOffset(reliableOffsets, 40.0);
    double maxDev = 0.0;
    for (double x : reliableOffsets)
        maxDev = std::max(maxDev, std::fabs(x - out.baseOffsetMs));
    out.maxDeviationMs = maxDev;

    // Drift = OLS slope of offset vs window time (ms per minute).
    if (reliableOffsets.size() >= 3) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        size_t k = 0;
        for (const auto& w : out.windows) {
            if (!w.reliable) continue;
            const double t = (w.startSeconds + w.endSeconds) / 2.0;
            sx += t; sy += reliableOffsets[k];
            sxx += t * t; sxy += t * reliableOffsets[k];
            ++k;
        }
        const double n = static_cast<double>(reliableOffsets.size());
        const double den = n * sxx - sx * sx;
        if (std::fabs(den) > 1e-9)
            out.driftMsPerMin = (n * sxy - sx * sy) / den * 60.0;
    }

    const int mismatch = static_cast<int>(out.windows.size()) - out.reliableCount;
    out.note = "分时段音频互相关 " + std::to_string(out.windows.size()) +
               " 窗，其中 " + std::to_string(out.reliableCount) +
               " 窗可靠（" +
               std::to_string(static_cast<int>(out.reliableRatio * 100)) +
               "%），基准偏移 " +
               std::to_string(static_cast<int>(out.baseOffsetMs * 10) / 10.0) +
               " ms，最大偏差 " +
               std::to_string(static_cast<int>(maxDev * 10) / 10.0) + " ms" +
               (mismatch > 0 ? "；" + std::to_string(mismatch) +
                                   " 窗因静音/波形不匹配未采信" : "");
    return out;
}

}  // namespace ma
