#pragma once
// ma::core - media DTOs shared between core, infra and any UI.
// The data model supports ANY number of streams of ANY type; nothing assumes
// "one video + one audio" (a structural bug of the legacy app).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ma {

enum class StreamKind { Video, Audio, Subtitle, Data, Attachment, Unknown };

inline const char* toString(StreamKind k) {
    switch (k) {
    case StreamKind::Video: return "video";
    case StreamKind::Audio: return "audio";
    case StreamKind::Subtitle: return "subtitle";
    case StreamKind::Data: return "data";
    case StreamKind::Attachment: return "attachment";
    case StreamKind::Unknown: return "unknown";
    }
    return "unknown";
}

struct StreamInfo {
    int index = 0;
    StreamKind kind = StreamKind::Unknown;

    std::string codecName;      // avcodec_get_name
    std::string codecLongName;
    std::string profile;        // may be empty -> UI must show "-" not crash
    int level = -1;

    // video
    int width = 0;
    int height = 0;
    std::string pixFmt;
    double frameRate = 0.0;     // avg_frame_rate, 0 = unknown

    // audio
    int channels = 0;
    int sampleRate = 0;
    std::string sampleFmt;

    // common
    std::string timeBase;       // "1/90000"
    double duration = 0.0;      // seconds, 0 = unknown
    int64_t nbFrames = -1;      // -1 = unknown
    int64_t bitrate = 0;
};

// One row of the frame table. Packet-level fields are always filled;
// decode-level extras only when the decode path was enabled.
struct FrameRecord {
    int64_t seq = 0;        // 1-based row number across the whole analysis
    int streamIndex = 0;

    int64_t dts = INT64_MIN;             // AV_NOPTS_VALUE -> INT64_MIN
    std::optional<double> dtsTime;
    std::optional<double> dtsDelta;      // delta vs previous packet of same stream

    int64_t pts = INT64_MIN;
    std::optional<double> ptsTime;

    int64_t duration = -1;               // in time_base units
    std::optional<double> durationTime;

    int64_t size = 0;
    bool keyframe = false;

    // Decode-only extras. These are geometry/format hints attached to the
    // packet row; they never replace packet-level timing, pictType or samples.
    bool decoded = false;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> pixFmt;
    std::optional<std::string> pictType;
    std::optional<std::string> sampleFmt;
    std::optional<int64_t> samples;
    std::optional<int> channels;
};

struct AnalysisResult {
    std::string formatName;       // "mov,mp4,m4a,3gp"
    std::string formatLongName;
    double duration = 0.0;        // container duration, seconds
    int64_t bitrate = 0;          // container bitrate, bit/s
    double startSeconds = 0.0;
    std::vector<StreamInfo> streams;
    std::vector<FrameRecord> frames;
    bool truncated = false;       // frames capped by options.maxFrames
    int64_t totalPacketsRead = 0;
};

} // namespace ma
