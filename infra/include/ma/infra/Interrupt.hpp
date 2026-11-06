#pragma once
// ma::infra - FFmpeg logging bridge + staged-timeout interrupt callback.
// Replaces the legacy "6 s then die" interrupt with independently
// configurable connect / stall timeouts and a cooperative cancel flag.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "ma/core/Analyzer.hpp"
#include "ma/core/Result.hpp"

struct AVFormatContext;

namespace ma::infra {

enum class LogLevel { Quiet, Panic, Fatal, Error, Warning, Info, Verbose, Debug, Trace };

// Installs an av_log callback that forwards to `sink` (thread-safe inside FFmpeg).
// Called implicitly by initFfmpeg().
void setLogSink(std::function<void(LogLevel, std::string)> sink);

// Idempotent process-wide init: avformat_network_init + log sink + version info.
void initFfmpeg();

// Staged interrupt context shared with AVIOInterruptCB.
class InterruptGuard {
public:
    struct Timeouts {
        double connectSeconds = 15.0; // until avformat_open_input returns
        double stallSeconds = 15.0;   // between two successful reads
        // Hard ceiling for avformat_find_stream_info. Probing a live HLS or a
        // high-latency RTSP source reads a lot of data but does NOT emit
        // packets, so the per-packet stall watchdog must not fire there.
        // <= 0 means "auto": max(60 s, 4 x stallSeconds).
        double probeSeconds = 0.0;
    };

    InterruptGuard(const ma::CancelFlag* cancel, Timeouts timeouts)
        : cancel_(cancel), timeouts_(timeouts) {
        if (timeouts_.probeSeconds <= 0.0)
            timeouts_.probeSeconds = std::max(60.0, timeouts_.stallSeconds * 4.0);
        connectStart_ = clock::now();
        lastActivity_ = connectStart_;
    }

    // Wire into a format context.
    void attach(AVFormatContext* fmt);

    // Called by the analyzer after open succeeded: switch to stall policing.
    void onOpened();
    // Called after each successful av_read_frame.
    void onActivity();
    // Bracket avformat_find_stream_info: during probing we police a single
    // generous deadline instead of the per-packet stall timeout.
    void beginProbe();
    void endProbe();
    bool inProbe() const { return probing_; }

    ma::Errc triggeredReason() const { return reason_; }
    const Timeouts& timeouts() const { return timeouts_; }

private:
    using clock = std::chrono::steady_clock;

    static int cbThunk(void* opaque);
    int check();

    const ma::CancelFlag* cancel_;
    Timeouts timeouts_;
    clock::time_point connectStart_;
    clock::time_point lastActivity_;
    clock::time_point probeStart_;
    bool opened_ = false;
    bool probing_ = false;
    ma::Errc reason_ = ma::Errc::Ok;
};

// Map AVERROR codes onto structured diagnostics; fills hint for known pitfalls.
ma::Diagnostic makeFfmpegDiagnostic(ma::Stage stage,
                                    int avErr,
                                    const std::string& url,
                                    const std::string& context);

} // namespace ma::infra
