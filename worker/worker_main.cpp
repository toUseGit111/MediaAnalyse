// analyzer-worker - the analysis kernel as a separate, crash-isolated process.
//
// Protocol (line-delimited JSON on stdin/stdout; logs on stderr):
//   GUI -> worker : {"type":"analyze","input":"...","options":{...}}
//   GUI -> worker : {"type":"cancel"}            (any time after the request)
//   worker -> GUI : {"type":"progress","packetsRead":N,"elapsed":T}   (streamed)
//   worker -> GUI : {"type":"streams",<toJson body, no frames>}       (after probe)
//   worker -> GUI : {"type":"frames",<toJson body, batch only>}       (streamed)
//   worker -> GUI : {"type":"result",<toJson body>}                    (final)
//
// The two snapshot lines exist because a live stream has no EOF: without them
// the GUI would show zero streams/zero rows until the user presses Stop. They
// reuse the exact same toJson()/fromJson() wire format as the final result.
//
// Because the kernel (FFmpeg + decoders) runs here and not in the GUI, any fault
// takes down only this process; the GUI keeps the scene, grabs the minidump and
// (once) restarts. See REFACTOR_PLAN.md P2.
//
// The worker has no console: everything worth debugging (request, FFmpeg's
// av_log, stage failures, final outcome) goes to logs/analyzer-worker.log.
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "ma/core/Analyzer.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/Serialization.hpp"
#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/Logger.hpp"
#include "ma/infra/ProbeAnalyzer.hpp"

namespace {

// Emit one JSON line to stdout and flush (pipes are not line-buffered).
void emitLine(const std::string& line) {
    std::cout << line << "\n" << std::flush;
}

}  // namespace

int main() {
    ma::infra::initLogger("analyzer-worker");
    ma::infra::installCrashHandler("analyzer-worker");
    // FFmpeg's own diagnostics are the single most useful thing in a support
    // bundle; route them into the log (redacted) as well as stderr.
    ma::infra::useFileLogSink();
    MA_LOGI("worker", "ready; FFmpeg=" << ma::infra::ffmpegVersionString());

    std::string request;
    if (!std::getline(std::cin, request)) {
        MA_LOGW("worker", "no request line on stdin - exiting");
        return 0;
    }
    MA_LOGD("worker", "request=" << ma::infra::redact(request));

    std::string input;
    ma::AnalysisOptions opts;
    if (!ma::core::parseAnalyzeRequest(request, input, opts) || input.empty()) {
        MA_LOGE("worker", "malformed or empty analyze request");
        ma::Diagnostic d;
        d.code = ma::Errc::InvalidArgument;
        d.stage = ma::Stage::NormalizeUrl;
        d.message = "malformed or empty analyze request";
        d.url = input;
        emitLine(ma::core::toJson(ma::AnalysisResult{}, d, /*withFrames=*/false,
                                  "result"));
        return 0;
    }

    MA_LOGI("worker", "analyze input=" << ma::infra::redact(input)
                       << " maxFrames=" << opts.maxFrames
                       << " decode=" << opts.decodeFrames
                       << " maxDecode=" << opts.maxDecodeFrames
                       << " connect=" << opts.connectTimeout
                       << " stall=" << opts.stallTimeout
                       << " probe=" << opts.probeTimeout
                       << " extraOptions=" << opts.extraOptions.size());

    std::atomic<bool> cancel{false};

    // A dedicated reader consumes any subsequent stdin line (a "cancel" command)
    // and flips the shared flag that the FFmpeg interrupt callback polls. It can
    // only start after the request line has been consumed by the main thread.
    std::thread cancelReader([&cancel]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (ma::core::jsonFieldString(line, "type") == "cancel") {
                MA_LOGI("worker", "cancel requested by GUI");
                cancel.store(true);
            }
        }
    });

    auto analyzer = ma::infra::createProbeAnalyzer();

    // Progress is high-frequency; sample it so a long stream does not produce
    // megabytes of near-identical lines.
    int progressLogTick = 0;
    ma::ProgressFn progress = [&progressLogTick](const ma::Progress& p) {
        if (++progressLogTick % 50 == 1)
            MA_LOGD("worker", "progress packets=" << p.packetsRead
                              << " elapsed=" << p.elapsedSeconds);
        emitLine("{\"type\":\"progress\",\"packetsRead\":" +
                 std::to_string(p.packetsRead) + ",\"elapsed\":" +
                 std::to_string(p.elapsedSeconds) + "}");
    };

    // Snapshot lines are additive: the GUI keeps rendering live rows while the
    // worker is still reading, and the final "result" line still carries the
    // canonical complete result. A Diagnostic with Errc::Ok keeps toJson()'s
    // "ok":true contract without inventing a new envelope format.
    ma::Diagnostic okDiag;
    okDiag.code = ma::Errc::Ok;
    okDiag.stage = ma::Stage::None;
    int snapshotLogTick = 0;
    ma::SnapshotFn snapshot = [&](const ma::AnalysisSnapshot& s) {
        if (s.hasStreams) {
            emitLine(ma::core::toJson(s.header, okDiag, /*withFrames=*/false,
                                      "streams"));
            MA_LOGI("worker", "streams snapshot streams="
                                  << s.header.streams.size());
        }
        if (!s.newFrames.empty()) {
            ma::AnalysisResult batch;
            batch.frames = s.newFrames;
            batch.totalPacketsRead = s.packetsRead;
            batch.timings.totalSeconds = s.elapsedSeconds;
            emitLine(ma::core::toJson(batch, okDiag, /*withFrames=*/true,
                                      "frames"));
            if (++snapshotLogTick % 25 == 1)
                MA_LOGD("worker", "frames snapshot rows="
                                      << s.newFrames.size()
                                      << " packets=" << s.packetsRead);
        }
    };

    ma::Outcome<ma::AnalysisResult> outcome;
    try {
        outcome = analyzer->analyze(input, opts, &cancel, progress, snapshot);
    } catch (const std::exception& e) {
        outcome.diag.code = ma::Errc::Internal;
        outcome.diag.stage = ma::Stage::Read;
        outcome.diag.message = std::string("worker exception: ") + e.what();
        outcome.diag.url = input;
        MA_LOGE("worker", "exception: " << e.what());
    } catch (...) {
        outcome.diag.code = ma::Errc::Internal;
        outcome.diag.stage = ma::Stage::Read;
        outcome.diag.message = "worker unknown exception";
        outcome.diag.url = input;
        MA_LOGE("worker", "unknown exception");
    }

    const auto& d = outcome.diag;
    MA_LOGI("worker", "result ok=" << outcome.ok()
                       << " code=" << ma::toString(d.code)
                       << " stage=" << ma::toString(d.stage)
                       << " native=" << d.nativeCode
                       << " message=" << ma::infra::redact(d.message)
                       << " hint=" << ma::infra::redact(d.hint)
                       << " url=" << ma::infra::redact(d.url)
                       << " streams=" << outcome.value.streams.size()
                       << " packets=" << outcome.value.totalPacketsRead
                       << " frames=" << outcome.value.frames.size()
                       << " elapsed=" << d.elapsedSeconds);
    if (!outcome.ok())
        MA_LOGW("worker", "failed diagnostic: " << ma::infra::redact(d.message));
    const auto& t = outcome.value.timings;
    MA_LOGI("worker", "timings open=" << t.openSeconds
                       << " probe=" << t.probeSeconds
                       << " demux=" << t.demuxSeconds
                       << " total=" << t.totalSeconds);

    // Always emit a final result line (ok or structured error). The GUI treats
    // a worker process that exits WITHOUT a result line as a crash.
    emitLine(ma::core::toJson(outcome.value, outcome.diag, /*withFrames=*/true,
                              "result"));

    // The reader thread is still blocked in std::getline() waiting for a
    // "cancel" line. Joining it would keep this process alive for as long as the
    // GUI holds the pipe open - i.e. one leaked worker per analysis. The result
    // line is already flushed, so leave immediately without unwinding: _Exit
    // avoids running static destructors while another thread is inside the C++
    // iostream implementation. Every log record has already been written to the
    // OS file handle, so no explicit logger shutdown is needed here.
    MA_LOGI("worker", "result delivered, exiting");
    std::cout.flush();
    std::_Exit(0);
}
