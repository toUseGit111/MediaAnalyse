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
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "ma/core/Alignment.hpp"
#include "ma/core/AudioCorrelation.hpp"
#include "ma/core/Analyzer.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/Serialization.hpp"
#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/Logger.hpp"
#include "ma/infra/ProbeAnalyzer.hpp"
#include "ma/infra/CstoreFetcher.hpp"
#include "ma/infra/AudioDecoder.hpp"

namespace {

// Emit one JSON line to stdout and flush (pipes are not line-buffered).
void emitLine(const std::string& line) {
    std::cout << line << "\n" << std::flush;
}

// Dual-source mode sends the second recording's snapshots/result through the
// same stdout channel. `source` lets the GUI route them to a second table
// model instead of overwriting the primary file's rows.
std::string withSource(const std::string& json, const char* source) {
    if (json.empty() || json.front() != '{') return json;
    return "{\"source\":\"" + std::string(source) + "\"," + json.substr(1);
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

    // cstore adapter: materialize the recording before the kernel sees it.
    // Fetch progress uses the same JSON-line channel so the GUI can show it.
    std::string fetchNotes;
    if (opts.source == "cstore") {
        MA_LOGI("worker", "cstore fetch key=" << ma::infra::redact(input));
        emitLine("{\"type\":\"fetch\",\"phase\":\"start\",\"done\":0,\"total\":0}");
        auto fetched = ma::infra::fetchCstoreRecording(
            input, opts, &cancel,
            [&](int done, int total, const std::string& phase) {
                emitLine("{\"type\":\"fetch\",\"phase\":\"" + phase +
                         "\",\"done\":" + std::to_string(done) +
                         ",\"total\":" + std::to_string(total) + "}");
            });
        if (!fetched.ok()) {
            ma::Outcome<ma::AnalysisResult> fail;
            fail.diag = fetched.diag;
            fail.diag.stage = ma::Stage::Open;
            MA_LOGE("worker", "cstore fetch failed: "
                                  << ma::infra::redact(fail.diag.message));
            emitLine(ma::core::toJson(fail.value, fail.diag, false, "result"));
            std::cout.flush();
            std::_Exit(0);
        }
        fetchNotes = ma::infra::describeFetch(*fetched);
        input = fetched->localPlaylist;
        MA_LOGI("worker", "cstore " << fetchNotes
                       << " elapsed=" << fetched->elapsedSeconds);
        emitLine("{\"type\":\"fetch\",\"phase\":\"done\",\"done\":" +
                 std::to_string(fetched->segmentsDownloaded) +
                 ",\"total\":" + std::to_string(fetched->segmentsTotal) +
                 ",\"note\":" + ma::core::jsonQuote(fetchNotes) + "}");
    }

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

    if (!fetchNotes.empty()) {
        outcome.diag.hint = fetchNotes +
                            (outcome.diag.hint.empty() ? "" : " | " + outcome.diag.hint);
    }

    // Optional cross-file alignment. The CLI still analyzes B silently and
    // only returns the comparison; GUI dual-source mode (compareFullOutput)
    // additionally streams B's snapshots and a full result tagged source:"B"
    // so the second recording gets its own frame table.
    ma::TimelineComparison comparison;
    const ma::TimelineComparison* cmpPtr = nullptr;
    if (!opts.compareInput.empty() && outcome.ok()) {
        auto cmpOpts = opts;
        cmpOpts.enableAlignment = true;
        cmpOpts.compareInput.clear();  // never recurse
        cmpOpts.hasAnchorMs = false;
        cmpOpts.anchorMs = 0.0;
        cmpOpts.source.clear();        // filled below only for a cstore key

        std::string secondInput = opts.compareInput;
        std::string secondFetchNotes;
        bool secondReady = true;

        if (opts.compareSource == "cstore") {
            MA_LOGI("worker", "cstore fetch B key="
                                  << ma::infra::redact(secondInput));
            emitLine(withSource(
                "{\"type\":\"fetch\",\"phase\":\"start\",\"done\":0,\"total\":0}",
                "B"));
            auto fetched = ma::infra::fetchCstoreRecording(
                secondInput, cmpOpts, &cancel,
                [](int done, int total, const std::string& phase) {
                    emitLine(withSource(
                        "{\"type\":\"fetch\",\"phase\":\"" + phase +
                            "\",\"done\":" + std::to_string(done) +
                            ",\"total\":" + std::to_string(total) + "}",
                        "B"));
                });
            if (!fetched.ok()) {
                secondReady = false;
                ma::Outcome<ma::AnalysisResult> fail;
                fail.diag = fetched.diag;
                fail.diag.stage = ma::Stage::Open;
                MA_LOGE("worker", "cstore fetch B failed: "
                                      << ma::infra::redact(fail.diag.message));
                if (opts.compareFullOutput)
                    emitLine(withSource(
                        ma::core::toJson(fail.value, fail.diag, false, "result"),
                        "B"));
            } else {
                secondFetchNotes = ma::infra::describeFetch(*fetched);
                secondInput = fetched->localPlaylist;
                emitLine(withSource(
                    "{\"type\":\"fetch\",\"phase\":\"done\",\"done\":" +
                        std::to_string(fetched->segmentsDownloaded) +
                        ",\"total\":" + std::to_string(fetched->segmentsTotal) +
                        ",\"note\":" + ma::core::jsonQuote(secondFetchNotes) + "}",
                    "B"));
            }
        }

        if (secondReady) {
            ma::ProgressFn secondProgress;
            ma::SnapshotFn secondSnapshot;
            if (opts.compareFullOutput) {
                secondProgress = [](const ma::Progress& p) {
                    emitLine(withSource(
                        "{\"type\":\"progress\",\"packetsRead\":" +
                            std::to_string(p.packetsRead) + ",\"elapsed\":" +
                            std::to_string(p.elapsedSeconds) + "}",
                        "B"));
                };
                secondSnapshot = [](const ma::AnalysisSnapshot& s) {
                    ma::Diagnostic ok;
                    ok.code = ma::Errc::Ok;
                    ok.stage = ma::Stage::None;
                    if (s.hasStreams)
                        emitLine(withSource(
                            ma::core::toJson(s.header, ok, false, "streams"),
                            "B"));
                    if (!s.newFrames.empty()) {
                        ma::AnalysisResult batch;
                        batch.frames = s.newFrames;
                        batch.totalPacketsRead = s.packetsRead;
                        batch.timings.totalSeconds = s.elapsedSeconds;
                        emitLine(withSource(
                            ma::core::toJson(batch, ok, true, "frames"), "B"));
                    }
                };
            }
            auto second = analyzer->analyze(secondInput, cmpOpts, &cancel,
                                            secondProgress, secondSnapshot);
            if (!secondFetchNotes.empty())
                second.diag.hint = secondFetchNotes +
                    (second.diag.hint.empty() ? "" : " | " + second.diag.hint);
            if (opts.compareFullOutput)
                emitLine(withSource(
                    ma::core::toJson(second.value, second.diag, true, "result",
                                     ma::infra::ffmpegVersionString(), 0),
                    "B"));

            const ma::AlignmentReport emptyA, emptyB;
            const ma::AlignmentReport& A =
                outcome.value.alignment ? *outcome.value.alignment : emptyA;
            const ma::AlignmentReport& B =
                second.value.alignment ? *second.value.alignment : emptyB;
            std::optional<double> anchor;
            if (opts.hasAnchorMs) anchor = opts.anchorMs;

            // Cross-view audio alignment: decode both tracks to 4 kHz mono
            // (small enough for a whole 40-min recording) and first get the
            // coarse global GCC-PHAT offset, then run a sliding-window track
            // to recover "who is ahead in which period" even when the two
            // recordings share one sound field but do not fully overlap.
            std::optional<ma::AudioCorrelationResult> audio;
            if (opts.audioAlign && outcome.ok() && second.ok()) {
                const int kRate = 4000;
                const double budget = std::clamp(opts.audioMaxSeconds, 1.0, 7200.0);
                auto aPcm = ma::infra::decodeAudioMono(
                    input, &cancel, opts, budget, kRate);
                auto bPcm = ma::infra::decodeAudioMono(
                    secondInput, &cancel, opts, budget, kRate);
                if (aPcm.ok() && bPcm.ok()) {
                    audio = ma::estimateAudioOffset(
                        *aPcm, *bPcm, kRate,
                        std::clamp(opts.audioMaxLagSeconds, 1.0, 300.0));
                    if (audio->ok) {
                        ma::AudioTrackOptions trackOpt;
                        trackOpt.windowSeconds = opts.audioWindowSeconds;
                        trackOpt.hopSeconds = opts.audioWindowSeconds;
                        trackOpt.localSearchSeconds =
                            std::clamp(opts.audioLocalSearchSeconds, 0.25, 30.0);
                        ma::AudioOffsetTrack track = ma::estimateAudioOffsetTrack(
                            *aPcm, *bPcm, kRate, audio->offsetMs, trackOpt);
                        if (track.ok) {
                            audio->hasTrack = true;
                            audio->offsetMs = track.baseOffsetMs;  // modal, robust
                            audio->track = std::move(track);
                        }
                    }
                    MA_LOGI("worker", "audio-xcorr ok=" << audio->ok
                                   << " offsetMs=" << audio->offsetMs
                                   << " track=" << audio->hasTrack
                                   << " windows="
                                   << (audio->hasTrack
                                           ? static_cast<int>(audio->track.windows.size())
                                           : 0)
                                   << " peak=" << audio->peakRatio
                                   << " conf=" << audio->confidence);
                } else {
                    MA_LOGW("worker", "audio-xcorr decode failed: "
                                   << ma::infra::redact(
                                          aPcm.ok() ? bPcm.diag.message
                                                    : aPcm.diag.message));
                }
            }

            comparison = ma::compareTimelines(A, B, anchor,
                                              &outcome.value, &second.value,
                                              audio ? &*audio : nullptr);
            cmpPtr = &comparison;
            MA_LOGI("worker", "compare method=" << comparison.method
                               << " offsetMs=" << comparison.offsetMs
                               << " confidence=" << comparison.confidence
                               << " secondOk=" << second.ok());
        }
    }

    // Always emit a final result line (ok or structured error). The GUI treats
    // a worker process that exits WITHOUT a result line as a crash.
    emitLine(ma::core::toJson(outcome.value, outcome.diag, /*withFrames=*/true,
                              "result", ma::infra::ffmpegVersionString(), 0,
                              cmpPtr));

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
