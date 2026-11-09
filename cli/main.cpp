// media-analyse-cli - command-line media analyzer (the GUI-free P1 deliverable).
// Usage:
//   media-analyse-cli [options] <url|file>
// Options:
//   --json                JSON output
//   --frames[=N]          print frame table (optionally capped at N rows)
//   --decode              attach frame-level width/height/pix_fmt/sample_fmt/channels
//                         to packet rows (never changes timing, type or samples)
//   --max-frames N        internal frame store cap (default 200000)
//   --max-decode-frames N real decode budget (default 3000)
//   --probesize B         FFmpeg probesize (bytes)
//   --analyze-duration US FFmpeg analyzeduration (microseconds)
//   --connect-timeout S   open-stage timeout, seconds
//   --stall-timeout S     between-packets timeout, seconds
//   --probe-timeout S     hard ceiling for find_stream_info (0 = auto)
//   --opt K=V             pass raw option to FFmpeg (repeatable)
//   --no-align            skip HLS timeline / A-V alignment post-processing
//   --compare <file>      analyze a second recording and report their offset;
//                         exact with #EXT-X-PROGRAM-DATE-TIME, otherwise a
//                         clearly-labelled relative/manual alignment
//   --anchor-ms N         manual sync point for --compare when PDT is absent
//                         (positive = primary starts later, in milliseconds)
// Exit codes: 0 ok, 1 failure (diagnostic printed), 2 usage error.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ma/core/Analyzer.hpp"
#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/Serialization.hpp"
#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/ProbeAnalyzer.hpp"
#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Logger.hpp"

namespace {

struct CliArgs {
    bool json = false;
    bool printFrames = false;
    int64_t printFrameLimit = 0; // 0 = all printed frames
    ma::AnalysisOptions opts;
    std::string input;
    std::string compare;                       // optional second recording
    std::optional<double> anchorMs;            // manual sync point
};

std::string optArg(const std::vector<std::string>& a, size_t& i) {
    auto pos = a[i].find('=');
    if (pos != std::string::npos) return a[i].substr(pos + 1);
    if (i + 1 < a.size()) return a[++i];
    return {};
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += b;
            } else {
                out += c;
            }
        }
    }
    return out;
}
std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string jnum(double v) {
    // JSON has no NaN/Infinity literals. FFmpeg's AV_NOPTS_VALUE-derived
    // sentinels can surface here for damaged or stream-only media.
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof(b), "%.6f", v);
    return b;
}

std::string jts(int64_t v) { return v == INT64_MIN ? std::string("null") : std::to_string(v); }

std::string jopt(const std::optional<double>& v) {
    return v ? jnum(*v) : "null";
}
std::string jopt(const std::optional<int64_t>& v) {
    return v ? std::to_string(*v) : "null";
}
std::string jopt(const std::optional<int>& v) {
    return v ? std::to_string(*v) : "null";
}
std::string jopt(const std::optional<std::string>& v) {
    return v ? jstr(*v) : "null";
}

// Render a stage timing, showing "-" when the stage was never reached (-1).
std::string fmtSec(double v) {
    if (v < 0) return "-";
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f s", v);
    return b;
}

void printHuman(const ma::AnalysisResult& r, const ma::Diagnostic& d,
                const CliArgs& args, const ma::TimelineComparison* cmp = nullptr) {
    std::printf("url used      : %s\n", d.url.c_str());
    if (!d.hint.empty()) std::printf("notes         : %s\n", d.hint.c_str());
    if (!r.formatName.empty())
        std::printf("format        : %s (%s)\n", r.formatName.c_str(),
                    r.formatLongName.c_str());
    std::printf("duration      : %.3f s   bitrate: %lld bit/s\n", r.duration,
                static_cast<long long>(r.bitrate));
    std::printf("streams       : %zu\n", r.streams.size());
    for (const auto& s : r.streams) {
        std::printf("  #%d %-9s codec=%s", s.index, ma::toString(s.kind),
                    s.codecName.c_str());
        if (!s.profile.empty()) std::printf(" profile=%s", s.profile.c_str());
        if (s.level >= 0) std::printf(" level=%d", s.level);
        if (s.kind == ma::StreamKind::Video) {
            std::printf(" %dx%d pix=%s fps=%.3f", s.width, s.height, s.pixFmt.c_str(),
                        s.frameRate);
        } else if (s.kind == ma::StreamKind::Audio) {
            std::printf(" ch=%d rate=%d fmt=%s", s.channels, s.sampleRate,
                        s.sampleFmt.c_str());
        }
        if (!s.timeBase.empty()) std::printf(" tb=%s", s.timeBase.c_str());
        if (s.duration > 0) std::printf(" dur=%.3f", s.duration);
        std::printf("\n");
    }
    std::printf("packets read  : %lld%s\n", static_cast<long long>(r.totalPacketsRead),
                r.truncated ? " (TRUNCATED)" : "");
    std::printf("timings       : open %s · probe %s · demux %s  (total %.3f s)\n",
                fmtSec(r.timings.openSeconds).c_str(),
                fmtSec(r.timings.probeSeconds).c_str(),
                fmtSec(r.timings.demuxSeconds).c_str(), r.timings.totalSeconds);

    if (args.printFrames) {
        int64_t shown = 0;
        for (const auto& f : r.frames) {
            if (args.printFrameLimit > 0 && shown >= args.printFrameLimit) break;
            std::printf("  #%lld [stream %d] dts=%lld dts_t=%s dts_d=%s pts=%lld"
                        " pts_t=%s dur=%lld size=%lld%s",
                        static_cast<long long>(f.seq), f.streamIndex,
                        f.dts == INT64_MIN ? -1LL : static_cast<long long>(f.dts),
                        jopt(f.dtsTime).c_str(), jopt(f.dtsDelta).c_str(),
                        f.pts == INT64_MIN ? -1LL : static_cast<long long>(f.pts),
                        jopt(f.ptsTime).c_str(), static_cast<long long>(f.duration),
                        static_cast<long long>(f.size), f.keyframe ? " [K]" : "");
            if (f.pictType) std::printf(" pic=%s", f.pictType->c_str());
            if (f.samples) std::printf(" samples=%lld", static_cast<long long>(*f.samples));
            if (f.decoded) std::printf(" [decoded]");
            if (f.width && f.height) std::printf(" %dx%d", *f.width, *f.height);
            if (f.pixFmt) std::printf(" pix=%s", f.pixFmt->c_str());
            if (f.sampleFmt) std::printf(" sfmt=%s", f.sampleFmt->c_str());
            if (f.channels) std::printf(" ch=%d", *f.channels);
            std::printf("\n");
            ++shown;
        }
        if (r.frames.empty()) std::printf("  (no frames stored)\n");
    }

    if (r.alignment) {
        const auto& a = *r.alignment;
        std::printf("alignment     : %s | cross-file=%s | covered %.3f s | "
                    "%d error, %d warning\n",
                    ma::toString(a.timeline),
                    a.crossFileComparable ? "YES (PDT)" : "no",
                    a.coveredSeconds, a.errorCount, a.warningCount);
        std::printf("  time base   : %s\n", a.timelineNote.c_str());
        if (a.firstWallClockMs)
            std::printf("  wall clock  : %s -> %s\n",
                        ma::formatEpochMsUtc(*a.firstWallClockMs).c_str(),
                        a.lastWallClockMs
                            ? ma::formatEpochMsUtc(*a.lastWallClockMs).c_str()
                            : "-");
        if (!a.playlist.segments.empty()) {
            int opened = 0;
            for (const auto& s : a.playlist.segments) if (s.opened) ++opened;
            std::printf("  segments    : %zu listed, %d opened, %d with PDT, "
                        "list duration %.3f s\n",
                        a.playlist.segments.size(), opened, a.playlist.pdtCount,
                        a.playlist.totalDuration());
        }
        for (const auto& s : a.streams)
            std::printf("  stream #%-2d  : %-8s %-8s offset=%+.1f ms "
                        "drift=%+.1f ms/min nonmono=%d gap=%.3f s\n",
                        s.streamIndex, ma::toString(s.kind), s.codec.c_str(),
                        s.startOffsetMs, s.driftMsPerMin,
                        s.nonMonotonicCount, s.maxGapSeconds);
        for (const auto& x : a.issues)
            std::printf("  [%s] %-26s %s\n", x.severity.c_str(), x.code.c_str(),
                        x.message.c_str());
    }
    if (cmp) {
        std::printf("comparison    : method=%s offset=%+.1f ms confidence=%.2f "
                    "overlap=%.3f s\n",
                    cmp->method.c_str(), cmp->offsetMs, cmp->confidence,
                    cmp->overlapSeconds);
        std::printf("  %s\n", cmp->note.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    ma::infra::initLogger("media-analyse-cli");
    ma::infra::installCrashHandler("media-analyse-cli");
    ma::infra::useFileLogSink();
    struct LogCloser {
        ~LogCloser() { ma::infra::closeLogger(); }
    } logCloser;
    (void)logCloser;

    std::vector<std::string> a(argv + 1, argv + argc);
    CliArgs args;
    {
        std::string cmdline;
        for (size_t k = 1; k < static_cast<size_t>(argc); ++k) {
            if (k > 1) cmdline += " ";
            cmdline += argv[k];
        }
        MA_LOGI("cli", "invocation: " << ma::infra::redact(cmdline)
                       << " | FFmpeg=" << ma::infra::ffmpegVersionString());
    }

    // Options may appear before OR after the primary input. Tokens after the
    // input that are not options are appended to it (unquoted Windows paths
    // with spaces), but --compare / --anchor-ms values are consumed explicitly
    // so they can never be swallowed into the path.
    bool inputSeen = false;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string& v = a[i];
        if (v == "--json") args.json = true;
        else if (v == "--frames") args.printFrames = true;
        else if (v.rfind("--frames=", 0) == 0) {
            args.printFrames = true;
            args.printFrameLimit = std::atoll(v.c_str() + 9);
        } else if (v == "--decode") args.opts.decodeFrames = true;
        else if (v == "--max-frames") args.opts.maxFrames = std::atoll(optArg(a, i).c_str());
        else if (v == "--max-decode-frames")
            args.opts.maxDecodeFrames = std::atoll(optArg(a, i).c_str());
        else if (v == "--probesize") args.opts.probesize = std::atoll(optArg(a, i).c_str());
        else if (v == "--analyze-duration")
            args.opts.analyzeduration = std::atoll(optArg(a, i).c_str());
        else if (v == "--connect-timeout")
            args.opts.connectTimeout = std::atof(optArg(a, i).c_str());
        else if (v == "--stall-timeout")
            args.opts.stallTimeout = std::atof(optArg(a, i).c_str());
        else if (v == "--probe-timeout")
            args.opts.probeTimeout = std::atof(optArg(a, i).c_str());
        else if (v == "--no-align") args.opts.enableAlignment = false;
        else if (v == "--compare" || v.rfind("--compare=", 0) == 0) {
            if (v.size() > 10) args.compare = v.substr(10);
            else if (i + 1 < a.size()) args.compare = a[++i];
            else { std::fprintf(stderr, "--compare needs a path\n"); return 2; }
        } else if (v == "--anchor-ms" || v.rfind("--anchor-ms=", 0) == 0) {
            std::string value = v.size() > 12 ? v.substr(12) : std::string();
            if (value.empty() && i + 1 < a.size()) value = a[++i];
            args.anchorMs = std::atof(value.c_str());
        } else if (v == "--opt") {
            std::string kv = optArg(a, i);
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                args.opts.extraOptions.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (v == "-h" || v == "--help") {
            std::puts("usage: media-analyse-cli [--json] [--frames[=N]] [--decode] "
                      "[--max-frames N] [--max-decode-frames N] "
                      "[--probesize B] [--analyze-duration US] "
                      "[--connect-timeout S] [--stall-timeout S] [--probe-timeout S] "
                      "[--opt K=V] [--no-align] [--compare <file>] "
                      "[--anchor-ms N] <url|file>");
            return 2;
        } else if (!v.empty() && v[0] == '-' && v.size() > 1) {
            std::fprintf(stderr, "unknown option: %s\n", v.c_str());
            return 2;
        } else if (!inputSeen) {
            args.input = v;
            inputSeen = true;
        } else {
            args.input += " " + v;
        }
    }

    if (args.input.empty()) {
        std::fprintf(stderr, "no input url/file given (see --help)\n");
        return 2;
    }

    MA_LOGI("cli", "analyze input=" << ma::infra::redact(args.input)
                    << " maxFrames=" << args.opts.maxFrames
                    << " decode=" << args.opts.decodeFrames
                    << " json=" << args.json
                    << " frames=" << args.printFrames);

    auto analyzer = ma::infra::createProbeAnalyzer();
    ma::CancelFlag cancel{false};
    auto outcome = analyzer->analyze(args.input, args.opts, &cancel, {});

    const auto& od = outcome.diag;
    MA_LOGI("cli", "result ok=" << outcome.ok()
                    << " code=" << ma::toString(od.code)
                    << " stage=" << ma::toString(od.stage)
                    << " native=" << od.nativeCode
                    << " message=" << ma::infra::redact(od.message)
                    << " url=" << ma::infra::redact(od.url)
                    << " packets=" << outcome.value.totalPacketsRead
                    << " elapsed=" << od.elapsedSeconds);

    ma::TimelineComparison comparison;
    const ma::TimelineComparison* cmpPtr = nullptr;
    if (!args.compare.empty()) {
        // The second recording must always carry a timeline, even if the
        // primary run was invoked with --no-align.
        auto cmpOpts = args.opts;
        cmpOpts.enableAlignment = true;
        auto second = analyzer->analyze(args.compare, cmpOpts, &cancel, {});
        const ma::AlignmentReport emptyA, emptyB;
        const ma::AlignmentReport& A =
            outcome.value.alignment ? *outcome.value.alignment : emptyA;
        const ma::AlignmentReport& B =
            second.value.alignment ? *second.value.alignment : emptyB;
        comparison = ma::compareTimelines(A, B, args.anchorMs);
        cmpPtr = &comparison;
        MA_LOGI("cli", "compare method=" << comparison.method
                        << " offsetMs=" << comparison.offsetMs
                        << " confidence=" << comparison.confidence
                        << " secondOk=" << second.ok());
    }

    if (args.json)
        std::fputs(ma::core::toJson(outcome.value, outcome.diag, args.printFrames, "",
                                    ma::infra::ffmpegVersionString(),
                                    args.printFrameLimit, cmpPtr).c_str(),
                   stdout);
    else
        printHuman(outcome.value, outcome.diag, args, cmpPtr);

    if (!outcome.ok()) {
        const auto& d = outcome.diag;
        std::fprintf(stderr, "FAILED[%s/%s]: %s (native=%d, %.2fs) url=%s%s%s\n",
                     ma::toString(d.code), ma::toString(d.stage), d.message.c_str(),
                     d.nativeCode, d.elapsedSeconds, d.url.c_str(),
                     d.hint.empty() ? "" : " hint=", d.hint.c_str());
        return 1;
    }
    return 0;
}
