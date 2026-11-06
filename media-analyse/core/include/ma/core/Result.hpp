#pragma once
// ma::core - Result / Diagnostic: every failure is data, never a bare bool.
#include <cstdint>
#include <string>

namespace ma {

enum class Stage {
    None = 0,
    NormalizeUrl,
    Open,
    FindStreamInfo,
    Read,
    Decode,
    Cancelled,
};

enum class Errc {
    Ok = 0,
    Cancelled,
    InvalidArgument,
    UnsupportedScheme,
    FileNotFound,
    Timeout,
    FfmpegError,
    Internal,
};

inline const char* toString(Stage s) {
    switch (s) {
    case Stage::None: return "none";
    case Stage::NormalizeUrl: return "normalize-url";
    case Stage::Open: return "open";
    case Stage::FindStreamInfo: return "find-stream-info";
    case Stage::Read: return "read";
    case Stage::Decode: return "decode";
    case Stage::Cancelled: return "cancelled";
    }
    return "unknown";
}

inline const char* toString(Errc e) {
    switch (e) {
    case Errc::Ok: return "ok";
    case Errc::Cancelled: return "cancelled";
    case Errc::InvalidArgument: return "invalid-argument";
    case Errc::UnsupportedScheme: return "unsupported-scheme";
    case Errc::FileNotFound: return "file-not-found";
    case Errc::Timeout: return "timeout";
    case Errc::FfmpegError: return "ffmpeg-error";
    case Errc::Internal: return "internal";
    }
    return "unknown";
}

struct Diagnostic {
    Errc code = Errc::Internal;
    Stage stage = Stage::None;
    int nativeCode = 0;        // AVERROR code when code == Errc::FfmpegError
    std::string message;       // av_strerror text or our own explanation
    std::string hint;          // actionable suggestion, may be empty
    std::string url;           // the final URL actually handed to FFmpeg
    double elapsedSeconds = 0.0;

    bool ok() const { return code == Errc::Ok; }
};

template <typename T>
struct Outcome {
    T value{};
    Diagnostic diag{};

    bool ok() const { return diag.ok(); }
    T* operator->() { return &value; }
    const T* operator->() const { return &value; }
    T& operator*() { return value; }
    const T& operator*() const { return value; }
};

} // namespace ma
