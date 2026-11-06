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
// Exit codes: 0 ok, 1 failure (diagnostic printed), 2 usage error.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ma/core/Analyzer.hpp"
#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"
#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/ProbeAnalyzer.hpp"

namespace {

struct CliArgs {
    bool json = false;
    bool printFrames = false;
    int64_t printFrameLimit = 0; // 0 = all printed frames
    ma::AnalysisOptions opts;
    std::string input;
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
                std::snprintf(b, sizeof(b), "\\u%04x", c);
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

void printHuman(const ma::AnalysisResult& r, const ma::Diagnostic& d, const CliArgs& args) {
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
}

void printJson(const ma::AnalysisResult& r, const ma::Diagnostic& d, const CliArgs& args) {
    std::printf("{\n  \"ok\": %s,\n", d.ok() ? "true" : "false");
    std::printf("  \"url\": %s,\n", jstr(d.url).c_str());
    std::printf("  \"elapsed\": %s,\n", jnum(d.elapsedSeconds).c_str());
    // Success hints (decode budget/flush stats, URL rewrites, stop notices)
    // used to exist only in human output. Expose them to --json consumers too.
    std::printf("  \"notes\": %s,\n", jstr(d.hint).c_str());
    if (!d.ok()) {
        std::printf("  \"error\": {\"code\": \"%s\", \"stage\": \"%s\", \"native\": %d, "
                    "\"message\": %s, \"hint\": %s},\n",
                    ma::toString(d.code), ma::toString(d.stage), d.nativeCode,
                    jstr(d.message).c_str(), jstr(d.hint).c_str());
    }
    std::printf("  \"format\": {\"name\": %s, \"longName\": %s, \"duration\": %s, "
                "\"bitrate\": %lld, \"start\": %s},\n",
                jstr(r.formatName).c_str(), jstr(r.formatLongName).c_str(),
                jnum(r.duration).c_str(), static_cast<long long>(r.bitrate),
                jnum(r.startSeconds).c_str());

    std::printf("  \"streams\": [\n");
    for (size_t i = 0; i < r.streams.size(); ++i) {
        const auto& s = r.streams[i];
        std::printf("    {\"index\": %d, \"kind\": \"%s\", \"codec\": %s, "
                    "\"profile\": %s, \"level\": %d, \"width\": %d, \"height\": %d, "
                    "\"pixFmt\": %s, \"frameRate\": %s, \"channels\": %d, "
                    "\"sampleRate\": %d, \"sampleFmt\": %s, \"timeBase\": %s, "
                    "\"duration\": %s}%s\n",
                    s.index, ma::toString(s.kind), jstr(s.codecName).c_str(),
                    jstr(s.profile).c_str(), s.level, s.width, s.height,
                    jstr(s.pixFmt).c_str(), jnum(s.frameRate).c_str(), s.channels,
                    s.sampleRate, jstr(s.sampleFmt).c_str(), jstr(s.timeBase).c_str(),
                    jnum(s.duration).c_str(), i + 1 < r.streams.size() ? "," : "");
    }
    std::printf("  ],\n");

    if (args.printFrames) {
        std::printf("  \"frames\": [\n");
        // Decide the comma from the number of rows actually EMITTED, not from
        // r.frames.size(): with --frames=N (N < total) the old logic left a
        // trailing comma and produced invalid JSON.
        const int64_t total = static_cast<int64_t>(r.frames.size());
        const int64_t visible =
            args.printFrameLimit > 0 ? std::min(total, args.printFrameLimit) : total;
        for (int64_t n = 0; n < visible; ++n) {
            const auto& f = r.frames[static_cast<size_t>(n)];
            std::printf("    {\"seq\": %lld, \"stream\": %d, \"dts\": %s, "
                        "\"dtsTime\": %s, \"dtsDelta\": %s, \"pts\": %s, "
                        "\"ptsTime\": %s, \"duration\": %lld, \"durationTime\": %s, "
                        "\"size\": %lld, \"key\": %s, \"pictType\": %s, "
                        "\"samples\": %s, \"decoded\": %s, "
                        "\"width\": %s, \"height\": %s, \"pixFmt\": %s, "
                        "\"sampleFmt\": %s, \"channels\": %s}%s\n",
                        static_cast<long long>(f.seq), f.streamIndex,
                        jts(f.dts).c_str(), jopt(f.dtsTime).c_str(),
                        jopt(f.dtsDelta).c_str(), jts(f.pts).c_str(),
                        jopt(f.ptsTime).c_str(), static_cast<long long>(f.duration),
                        jopt(f.durationTime).c_str(), static_cast<long long>(f.size),
                        f.keyframe ? "true" : "false", jopt(f.pictType).c_str(),
                        jopt(f.samples).c_str(), f.decoded ? "true" : "false",
                        jopt(f.width).c_str(), jopt(f.height).c_str(),
                        jopt(f.pixFmt).c_str(), jopt(f.sampleFmt).c_str(),
                        jopt(f.channels).c_str(), n + 1 < visible ? "," : "");
        }
        std::printf("  ],\n");
    }
    std::printf("  \"totalPackets\": %lld,\n", static_cast<long long>(r.totalPacketsRead));
    std::printf("  \"truncated\": %s,\n", r.truncated ? "true" : "false");
    std::printf("  \"ffmpeg\": \"%s\"\n}\n", ma::infra::ffmpegVersionString());
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    CliArgs args;

    size_t i = 0;
    for (; i < a.size(); ++i) {
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
        else if (v == "--opt") {
            std::string kv = optArg(a, i);
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                args.opts.extraOptions.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (v == "-h" || v == "--help") {
            std::puts("usage: media-analyse-cli [--json] [--frames[=N]] [--decode] "
                      "[--max-frames N] [--max-decode-frames N] "
                      "[--probesize B] [--analyze-duration US] "
                      "[--connect-timeout S] [--stall-timeout S] [--probe-timeout S] "
                      "[--opt K=V] <url|file>");
            return 2;
        } else if (!v.empty() && v[0] == '-' && v.size() > 1) {
            std::fprintf(stderr, "unknown option: %s\n", v.c_str());
            return 2;
        } else {
            args.input = v;
            ++i;
            break;
        }
    }
    if (i < a.size()) args.input += " " + a[i]; // tolerate unquoted spaces in paths

    if (args.input.empty()) {
        std::fprintf(stderr, "no input url/file given (see --help)\n");
        return 2;
    }

    auto analyzer = ma::infra::createProbeAnalyzer();
    ma::CancelFlag cancel{false};
    auto outcome = analyzer->analyze(args.input, args.opts, &cancel, {});

    if (args.json)
        printJson(outcome.value, outcome.diag, args);
    else
        printHuman(outcome.value, outcome.diag, args);

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
