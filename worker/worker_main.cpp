// analyzer-worker - the analysis kernel as a separate, crash-isolated process.
//
// Protocol (line-delimited JSON on stdin/stdout; logs on stderr):
//   GUI -> worker : {"type":"analyze","input":"...","options":{...}}
//   GUI -> worker : {"type":"cancel"}            (any time after the request)
//   worker -> GUI : {"type":"progress","packetsRead":N,"elapsed":T}   (streamed)
//   worker -> GUI : {"type":"result",<toJson body>}                    (final)
//
// Because the kernel (FFmpeg + decoders) runs here and not in the GUI, any fault
// takes down only this process; the GUI keeps the scene, grabs the minidump and
// (once) restarts. See REFACTOR_PLAN.md P2.
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
#include "ma/infra/ProbeAnalyzer.hpp"

namespace {

// Emit one JSON line to stdout and flush (pipes are not line-buffered).
void emitLine(const std::string& line) {
    std::cout << line << "\n" << std::flush;
}

} // namespace

int main() {
    ma::infra::installCrashHandler("analyzer-worker");

    std::string request;
    if (!std::getline(std::cin, request)) {
        // No request: nothing to do.
        return 0;
    }

    std::string input;
    ma::AnalysisOptions opts;
    if (!ma::core::parseAnalyzeRequest(request, input, opts) || input.empty()) {
        ma::Diagnostic d;
        d.code = ma::Errc::InvalidArgument;
        d.stage = ma::Stage::NormalizeUrl;
        d.message = "malformed or empty analyze request";
        d.url = input;
        emitLine(ma::core::toJson(ma::AnalysisResult{}, d, /*withFrames=*/false,
                                  "result"));
        return 0;
    }

    std::atomic<bool> cancel{false};

    // A dedicated reader consumes any subsequent stdin line (a "cancel" command)
    // and flips the shared flag that the FFmpeg interrupt callback polls. It can
    // only start after the request line has been consumed by the main thread.
    std::thread cancelReader([&cancel]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (ma::core::jsonFieldString(line, "type") == "cancel") {
                cancel.store(true);
            }
        }
    });

    auto analyzer = ma::infra::createProbeAnalyzer();

    ma::ProgressFn progress = [](const ma::Progress& p) {
        emitLine("{\"type\":\"progress\",\"packetsRead\":" +
                 std::to_string(p.packetsRead) + ",\"elapsed\":" +
                 std::to_string(p.elapsedSeconds) + "}");
    };

    ma::Outcome<ma::AnalysisResult> outcome;
    try {
        outcome = analyzer->analyze(input, opts, &cancel, progress);
    } catch (const std::exception& e) {
        outcome.diag.code = ma::Errc::Internal;
        outcome.diag.stage = ma::Stage::Read;
        outcome.diag.message = std::string("worker exception: ") + e.what();
        outcome.diag.url = input;
    } catch (...) {
        outcome.diag.code = ma::Errc::Internal;
        outcome.diag.stage = ma::Stage::Read;
        outcome.diag.message = "worker unknown exception";
        outcome.diag.url = input;
    }

    // Always emit a final result line (ok or structured error). The GUI treats
    // a worker process that exits WITHOUT a result line as a crash.
    emitLine(ma::core::toJson(outcome.value, outcome.diag, /*withFrames=*/true,
                              "result"));

    // The reader thread is still blocked in std::getline() waiting for a
    // "cancel" line. Joining it would keep this process alive for as long as the
    // GUI holds the pipe open - i.e. one leaked worker per analysis. The result
    // line is already flushed, so leave immediately without unwinding: _Exit
    // avoids running static destructors while another thread is inside the C++
    // iostream implementation.
    std::cout.flush();
    std::_Exit(0);
}
