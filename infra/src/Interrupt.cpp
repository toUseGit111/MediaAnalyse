// ma::infra - interrupt callback, log bridge, error mapping. Implementation.
#include "ma/infra/Interrupt.hpp"

#include <cstdio>
#include <mutex>
#include <string>

#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Logger.hpp"

namespace ma::infra {

namespace {

std::function<void(LogLevel, std::string)> g_sink;
std::mutex g_sinkMutex;

void avLogThunk(void* /*avcl*/, int level, const char* fmt, va_list vl) {
    // MA_LOG_LEVEL=debug also unlocks FFmpeg's verbose/debug diagnostics; the
    // default stays at INFO so a long stream cannot flood the log file.
    const int ceiling =
        (logLevel() <= Severity::Debug) ? AV_LOG_DEBUG : AV_LOG_INFO;
    if (level > ceiling) return;
    char buf[2048];
    int prefix = 1;
    av_log_format_line2(nullptr, level, fmt, vl, buf, sizeof(buf), &prefix);
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_sink) {
        LogLevel mapped = LogLevel::Info;
        if (level <= AV_LOG_PANIC) mapped = LogLevel::Panic;
        else if (level <= AV_LOG_FATAL) mapped = LogLevel::Fatal;
        else if (level <= AV_LOG_ERROR) mapped = LogLevel::Error;
        else if (level <= AV_LOG_WARNING) mapped = LogLevel::Warning;
        else if (level <= AV_LOG_INFO) mapped = LogLevel::Info;
        else if (level <= AV_LOG_VERBOSE) mapped = LogLevel::Verbose;
        else mapped = LogLevel::Debug;
        g_sink(mapped, std::string(buf));
    }
}

} // namespace

void setLogSink(std::function<void(LogLevel, std::string)> sink) {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink = std::move(sink);
    av_log_set_callback(avLogThunk);
}

namespace {
Severity severityFor(LogLevel lv) {
    switch (lv) {
    case LogLevel::Panic:
    case LogLevel::Fatal: return Severity::Fatal;
    case LogLevel::Error: return Severity::Error;
    case LogLevel::Warning: return Severity::Warning;
    case LogLevel::Info: return Severity::Info;
    default: return Severity::Debug;
    }
}
}  // namespace

void useFileLogSink() {
    setLogSink([](LogLevel lv, std::string msg) {
        // Keep the console behaviour for CLI users, AND make the same text
        // recoverable from the shipped (console-less) GUI build.
        std::fputs(msg.c_str(), stderr);
        logMessage(severityFor(lv), "ffmpeg", redact(msg));
    });
}

void initFfmpeg() {
    static bool done = false;
    if (done) return;
    done = true;
    avformat_network_init();
    if (!g_sink) useFileLogSink();
}

const char* ffmpegVersionString() {
    // Runtime version info string from libavutil.
    return av_version_info();
}

// ---------------- InterruptGuard ----------------

int InterruptGuard::cbThunk(void* opaque) {
    auto* self = static_cast<InterruptGuard*>(opaque);
    return self ? self->check() : 0;
}

void InterruptGuard::attach(AVFormatContext* fmt) {
    if (fmt) fmt->interrupt_callback = {&InterruptGuard::cbThunk, this};
}

void InterruptGuard::onOpened() {
    opened_ = true;
    lastActivity_ = clock::now();
}

void InterruptGuard::onActivity() { lastActivity_ = clock::now(); }

void InterruptGuard::beginProbe() {
    probing_ = true;
    probeStart_ = clock::now();
    lastActivity_ = probeStart_;
}

void InterruptGuard::endProbe() {
    probing_ = false;
    // A long probe must not immediately trip the stall watchdog on the very
    // first av_read_frame: the read loop starts from a fresh activity stamp.
    lastActivity_ = clock::now();
}

int InterruptGuard::check() {
    // Cancel always wins, including over a timeout that would otherwise fire
    // in the same callback. This is what makes "Stop" protocol-independent.
    if (cancel_ && cancel_->load(std::memory_order_relaxed)) {
        reason_ = ma::Errc::Cancelled;
        return 1;
    }
    const auto now = clock::now();
    if (probing_) {
        // find_stream_info can spend tens of seconds reading probe bytes
        // without ever producing a packet. Police one generous deadline.
        const double elapsed =
            std::chrono::duration<double>(now - probeStart_).count();
        if (timeouts_.probeSeconds > 0 && elapsed > timeouts_.probeSeconds) {
            reason_ = ma::Errc::Timeout;
            return 1;
        }
        return 0;
    }
    if (!opened_) {
        const double elapsed =
            std::chrono::duration<double>(now - connectStart_).count();
        if (timeouts_.connectSeconds > 0 && elapsed > timeouts_.connectSeconds) {
            reason_ = ma::Errc::Timeout;
            return 1;
        }
    } else {
        const double idle =
            std::chrono::duration<double>(now - lastActivity_).count();
        if (timeouts_.stallSeconds > 0 && idle > timeouts_.stallSeconds) {
            reason_ = ma::Errc::Timeout;
            return 1;
        }
    }
    return 0;
}

// ---------------- error mapping ----------------

Diagnostic makeFfmpegDiagnostic(Stage stage, int avErr, const std::string& url,
                                const std::string& context) {
    Diagnostic d;
    d.stage = stage;
    d.nativeCode = avErr;
    d.url = url;

    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (avErr == AVERROR_EXIT) {
        // Interrupt callback fired.
        d.code = Errc::Cancelled;
        d.message = "interrupted";
    } else {
        av_strerror(avErr, buf, sizeof(buf));
        d.message = buf;
        d.code = Errc::FfmpegError;
    }

    if (avErr == AVERROR(ENOENT)) {
        d.code = Errc::FileNotFound;
        d.hint = "file or resource does not exist; check the path";
    } else if (avErr == AVERROR(EINVAL)) {
        d.hint = "invalid argument - check URL syntax and options";
    } else if (avErr == AVERROR(EIO)) {
        d.hint = "I/O error - connection dropped or media unreadable";
    } else if (avErr == AVERROR_STREAM_NOT_FOUND) {
        d.hint = "no usable stream found inside the container";
    } else if (avErr == AVERROR_PROTOCOL_NOT_FOUND) {
        d.hint = "protocol not supported by this build; check protocol_whitelist";
    } else if (avErr == AVERROR_HTTP_FORBIDDEN) {
        d.hint = "HTTP 403 - authentication/referer/UA may be required";
    } else if (avErr == AVERROR_HTTP_NOT_FOUND) {
        d.hint = "HTTP 404 - resource missing";
    } else if (avErr == AVERROR_HTTP_OTHER_4XX || avErr == AVERROR_HTTP_BAD_REQUEST) {
        d.hint = "HTTP 4xx client error";
    } else if (avErr == AVERROR_HTTP_UNAUTHORIZED) {
        d.hint = "HTTP 401 - credentials required";
    }

    if (!context.empty()) {
        d.message += " [";
        d.message += context;
        d.message += "]";
    }
    if (d.code == Errc::Cancelled && avErr == AVERROR_EXIT) {
        // Distinguish timeout vs cancel is done by the caller comparing reason_.
    }
    return d;
}

} // namespace ma::infra
