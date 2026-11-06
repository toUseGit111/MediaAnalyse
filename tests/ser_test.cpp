// Round-trip / envelope tests for ma::core serialization. Links only ma_core
// (no FFmpeg, no Qt) so it builds and runs in well under a second.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/Serialization.hpp"

#define CHECK(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main() {
    using namespace ma;

    // ---- build a representative result ----
    AnalysisResult r;
    r.formatName = "mov,mp4,m4a,3gp";
    r.formatLongName = "QuickTime / MOV";
    r.duration = 10.5;
    r.bitrate = 123456;
    r.startSeconds = 0.0;
    r.totalPacketsRead = 1190;
    r.truncated = false;
    // Exact binary fractions so %.6f serialization round-trips bit-for-bit.
    r.timings.openSeconds = 0.5;
    r.timings.probeSeconds = 0.25;
    r.timings.demuxSeconds = 0.125;
    r.timings.totalSeconds = 0.875;

    StreamInfo s0;
    s0.index = 0; s0.kind = StreamKind::Video; s0.codecName = "h264";
    s0.profile = "Main"; s0.level = 30; s0.width = 320; s0.height = 176;
    s0.pixFmt = "yuv420p"; s0.frameRate = 25.0; s0.timeBase = "1/90000";
    r.streams.push_back(s0);

    StreamInfo s1;
    s1.index = 1; s1.kind = StreamKind::Audio; s1.codecName = "aac";
    s1.channels = 2; s1.sampleRate = 48000; s1.sampleFmt = "fltp";
    s1.timeBase = "1/48000"; s1.duration = 9.88;
    r.streams.push_back(s1);

    FrameRecord f;
    f.seq = 1; f.streamIndex = 0; f.dts = 0; f.dtsTime = 0.0; f.pts = 0;
    f.ptsTime = 0.0; f.duration = 3600; f.durationTime = 0.04; f.size = 1234;
    f.keyframe = true; f.pictType = std::string("I");
    f.decoded = true; f.width = 320; f.height = 176; f.pixFmt = std::string("yuv420p");
    r.frames.push_back(f);

    FrameRecord f2;
    f2.seq = 2; f2.streamIndex = 1; f2.dts = INT64_MIN;  // null sentinel
    f2.samples = std::optional<int64_t>(1024); f2.channels = std::optional<int>(2);
    f2.sampleFmt = std::string("fltp"); f2.decoded = false;
    r.frames.push_back(f2);

    Diagnostic d;
    d.code = Errc::Ok; d.stage = Stage::Read; d.nativeCode = 0;
    d.message = "ok"; d.hint = "a hint with \"quotes\" and \\ backslash";
    d.url = "file:C:/tmp/sample.mp4"; d.elapsedSeconds = 1.234567;

    // ---- round trip (with frames) ----
    std::string json = ma::core::toJson(r, d, /*withFrames=*/true, "result");
    AnalysisResult r2;
    Diagnostic d2;
    CHECK(ma::core::fromJson(json, r2, d2));

    CHECK(r2.formatName == r.formatName);
    CHECK(r2.duration == r.duration);
    CHECK(r2.streams.size() == r.streams.size());
    CHECK(r2.streams[0].codecName == "h264");
    CHECK(r2.streams[0].width == 320);
    CHECK(r2.streams[1].channels == 2);
    CHECK(r2.frames.size() == 2);
    CHECK(r2.totalPacketsRead == 1190);
    CHECK(d2.code == Errc::Ok);
    CHECK(d2.url == d.url);
    CHECK(d2.message == d.message);  // escaped chars preserved
    CHECK(r2.frames[0].pictType && *r2.frames[0].pictType == "I");
    CHECK(r2.frames[0].width && *r2.frames[0].width == 320);
    // null sentinel round trips to INT64_MIN / nullopt
    CHECK(r2.frames[1].dts == INT64_MIN);
    CHECK(r2.frames[1].samples && *r2.frames[1].samples == 1024);
    CHECK(r2.frames[1].pictType == std::nullopt);
    // stage timings round trip exactly
    CHECK(r2.timings.openSeconds == 0.5);
    CHECK(r2.timings.probeSeconds == 0.25);
    CHECK(r2.timings.demuxSeconds == 0.125);
    CHECK(r2.timings.totalSeconds == 0.875);

    // ---- without frames flag omits "frames" ----
    std::string jsonNoFrames = ma::core::toJson(r, d, /*withFrames=*/false, "result");
    CHECK(jsonNoFrames.find("\"frames\"") == std::string::npos);
    AnalysisResult r3; Diagnostic d3;
    CHECK(ma::core::fromJson(jsonNoFrames, r3, d3));
    CHECK(r3.frames.empty());

    // ---- unreached stage (-1) serializes as null and parses back to -1 ----
    AnalysisResult rn; Diagnostic dn;
    std::string jn = ma::core::toJson(rn, dn, /*withFrames=*/false, "result");
    CHECK(jn.find("\"open\":null") != std::string::npos);
    AnalysisResult rn2; Diagnostic dn2;
    CHECK(ma::core::fromJson(jn, rn2, dn2));
    CHECK(rn2.timings.openSeconds == -1.0);
    CHECK(rn2.timings.probeSeconds == -1.0);
    CHECK(rn2.timings.demuxSeconds == -1.0);

    // ---- options round trip + request envelope ----
    AnalysisOptions o;
    o.connectTimeout = 20.0; o.stallTimeout = 10.0; o.decodeFrames = true;
    o.extraOptions.emplace_back("rtsp_transport", "tcp");
    o.extraOptions.emplace_back("headers", "User-Agent: x\nReferer: y");
    std::string oj = ma::core::toOptionsJson(o);
    AnalysisOptions o2;
    CHECK(ma::core::fromOptionsJson(oj, o2));
    CHECK(o2.connectTimeout == 20.0);
    CHECK(o2.decodeFrames == true);
    CHECK(o2.extraOptions.size() == 2);
    CHECK(o2.extraOptions[0].first == "rtsp_transport");
    CHECK(o2.extraOptions[1].second == "User-Agent: x\nReferer: y");

    std::string req = "{\"type\":\"analyze\",\"input\":" +
                      ma::core::jsonQuote("file:C:/tmp/sample.mp4?a=b") +
                      ",\"options\":" + oj + "}";
    CHECK(ma::core::jsonFieldString(req, "type") == "analyze");
    std::string in; AnalysisOptions o3;
    CHECK(ma::core::parseAnalyzeRequest(req, in, o3));
    CHECK(in == "file:C:/tmp/sample.mp4?a=b");
    CHECK(o3.decodeFrames == true);

    std::printf("ser_test: ALL PASS\n");
    return 0;
}
