// ma::core - JSON (de)serialization. See Serialization.hpp for the contract.
// A minimal recursive-descent parser backs both toJson()/fromJson() so the
// wire format stays consistent across CLI, worker and GUI without pulling in a
// third-party JSON library.
#include "ma/core/Serialization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace ma::core {
namespace {

// ---- internal JSON value + parser ------------------------------------------

struct JVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double number = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    bool isNull() const { return type == Null; }
    bool isObject() const { return type == Object; }
    bool isArray() const { return type == Array; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    bool isBool() const { return type == Bool; }
    long long asInt() const { return type == Number ? static_cast<long long>(number) : 0; }
    double asDouble() const { return type == Number ? number : 0.0; }
    bool asBool() const { return type == Bool ? b : false; }
    std::string asString() const { return type == String ? str : std::string(); }

    const JVal* find(const std::string& k) const {
        if (type != Object) return nullptr;
        for (const auto& p : obj)
            if (p.first == k) return &p.second;
        return nullptr;
    }
};

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& str) : s(str) {}

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }

    JVal parse() {
        skipWs();
        return parseValue();
    }

    JVal parseValue() {
        skipWs();
        JVal v;
        if (i >= s.size()) return v;  // Null
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { v.type = JVal::String; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { i += 4; return v; }  // null
        return parseNumber();
    }

    JVal parseObject() {
        JVal v; v.type = JVal::Object;
        ++i;  // consume '{'
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return v; }
        while (true) {
            skipWs();
            std::string key;
            if (i < s.size() && s[i] == '"') key = parseString();
            skipWs();
            if (i < s.size() && s[i] == ':') ++i;
            JVal val = parseValue();
            v.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            break;
        }
        return v;
    }

    JVal parseArray() {
        JVal v; v.type = JVal::Array;
        ++i;  // consume '['
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return v; }
        while (true) {
            v.arr.push_back(parseValue());
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; break; }
            break;
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        ++i;  // consume opening '"'
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) break;
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 <= s.size()) {
                            unsigned cp = 0;
                            for (int k = 0; k < 4; ++k) {
                                char h = s[i++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= h - '0';
                                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            }
                            if (cp < 0x80) out += static_cast<char>(cp);
                            else if (cp < 0x800) {
                                out += static_cast<char>(0xC0 | (cp >> 6));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (cp >> 12));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    JVal parseNumber() {
        JVal v; v.type = JVal::Number;
        size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                c == 'e' || c == 'E')
                ++i;
            else break;
        }
        try { v.number = std::stod(s.substr(start, i - start)); }
        catch (...) { v.number = 0.0; }
        return v;
    }

    JVal parseBool() {
        JVal v; v.type = JVal::Bool;
        if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; }
        else { v.b = false; i += 5; }
        return v;
    }
};

JVal parseJsonOrNull(const std::string& s) {
    Parser p(s);
    return p.parse();
}

// ---- writers ---------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
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
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof(b), "%.6f", v);
    return b;
}

std::string jint64(int64_t v) {
    return v == INT64_MIN ? std::string("null") : std::to_string(v);
}

std::string joptD(const std::optional<double>& v) {
    return v ? jnum(*v) : "null";
}
std::string joptI64(const std::optional<int64_t>& v) {
    return v ? std::to_string(*v) : "null";
}
std::string joptI(const std::optional<int>& v) {
    return v ? std::to_string(*v) : "null";
}
std::string joptS(const std::optional<std::string>& v) {
    return v ? jstr(*v) : "null";
}
// A stage that never ran is -1.0 in the DTO; emit JSON null so consumers can
// tell "not reached" from a real-but-fast zero.
std::string joptSec(double v) { return v < 0.0 ? std::string("null") : jnum(v); }

// ---- string <-> enum helpers ----------------------------------------------

StreamKind kindFromString(const std::string& k) {
    if (k == "video") return StreamKind::Video;
    if (k == "audio") return StreamKind::Audio;
    if (k == "subtitle") return StreamKind::Subtitle;
    if (k == "data") return StreamKind::Data;
    if (k == "attachment") return StreamKind::Attachment;
    return StreamKind::Unknown;
}
Stage stageFromString(const std::string& s) {
    if (s == "normalize-url") return Stage::NormalizeUrl;
    if (s == "open") return Stage::Open;
    if (s == "find-stream-info") return Stage::FindStreamInfo;
    if (s == "read") return Stage::Read;
    if (s == "decode") return Stage::Decode;
    if (s == "cancelled") return Stage::Cancelled;
    return Stage::None;
}
Errc errcFromString(const std::string& s) {
    if (s == "cancelled") return Errc::Cancelled;
    if (s == "invalid-argument") return Errc::InvalidArgument;
    if (s == "unsupported-scheme") return Errc::UnsupportedScheme;
    if (s == "file-not-found") return Errc::FileNotFound;
    if (s == "timeout") return Errc::Timeout;
    if (s == "ffmpeg-error") return Errc::FfmpegError;
    if (s == "internal") return Errc::Internal;
    return Errc::Ok;
}

} // namespace

// ---- public API ------------------------------------------------------------

std::string toJson(const AnalysisResult& r, const Diagnostic& d, bool withFrames,
                   const std::string& type, const std::string& ffmpegVersion,
                   int64_t frameLimit) {
    std::string o;
    o.reserve(4096);
    o += "{";
    if (!type.empty()) o += "\"type\":" + jstr(type) + ",";
    o += "\"ok\":" + std::string(d.ok() ? "true" : "false") + ",";
    o += "\"diagnostic\":{";
    o += "\"code\":" + jstr(toString(d.code)) + ",";
    o += "\"stage\":" + jstr(toString(d.stage)) + ",";
    o += "\"native\":" + std::to_string(d.nativeCode) + ",";
    o += "\"message\":" + jstr(d.message) + ",";
    o += "\"hint\":" + jstr(d.hint) + ",";
    o += "\"url\":" + jstr(d.url) + ",";
    o += "\"elapsed\":" + jnum(d.elapsedSeconds);
    o += "},";

    // Backwards-compatible aliases for the documented CLI --json contract:
    // scripts read top-level "notes" and "error" (canonical fields remain under
    // "diagnostic"). Both ends live in this repo, so the extra fields are cheap
    // and keep external parsing stable.
    o += "\"notes\":" + jstr(d.hint) + ",";
    if (!d.ok()) {
        o += "\"error\":{\"code\":" + jstr(toString(d.code)) +
             ",\"stage\":" + jstr(toString(d.stage)) +
             ",\"native\":" + std::to_string(d.nativeCode) + "},";
    }

    o += "\"format\":{\"name\":" + jstr(r.formatName) + ",\"longName\":" +
         jstr(r.formatLongName) + ",\"duration\":" + jnum(r.duration) +
         ",\"bitrate\":" + std::to_string(r.bitrate) +
         ",\"start\":" + jnum(r.startSeconds) + "},";

    // Stage timing breakdown (open / find_stream_info / demux). null = the
    // stage was never reached (e.g. open failed before probing).
    o += "\"timings\":{\"open\":" + joptSec(r.timings.openSeconds) +
         ",\"probe\":" + joptSec(r.timings.probeSeconds) +
         ",\"demux\":" + joptSec(r.timings.demuxSeconds) +
         ",\"total\":" + jnum(r.timings.totalSeconds) + "},";

    o += "\"streams\":[";
    for (size_t i = 0; i < r.streams.size(); ++i) {
        const auto& s = r.streams[i];
        o += "{";
        o += "\"index\":" + std::to_string(s.index) + ",";
        o += "\"kind\":" + jstr(toString(s.kind)) + ",";
        o += "\"codec\":" + jstr(s.codecName) + ",";
        o += "\"profile\":" + jstr(s.profile) + ",";
        o += "\"level\":" + std::to_string(s.level) + ",";
        o += "\"width\":" + std::to_string(s.width) + ",";
        o += "\"height\":" + std::to_string(s.height) + ",";
        o += "\"pixFmt\":" + jstr(s.pixFmt) + ",";
        o += "\"frameRate\":" + jnum(s.frameRate) + ",";
        o += "\"channels\":" + std::to_string(s.channels) + ",";
        o += "\"sampleRate\":" + std::to_string(s.sampleRate) + ",";
        o += "\"sampleFmt\":" + jstr(s.sampleFmt) + ",";
        o += "\"timeBase\":" + jstr(s.timeBase) + ",";
        o += "\"duration\":" + jnum(s.duration);
        o += "}";
        if (i + 1 < r.streams.size()) o += ",";
    }
    o += "]";

    if (withFrames) {
        size_t shown = r.frames.size();
        if (frameLimit > 0 && static_cast<size_t>(frameLimit) < shown)
            shown = static_cast<size_t>(frameLimit);
        o += ",\"frames\":[";
        for (size_t i = 0; i < shown; ++i) {
            const auto& f = r.frames[i];
            o += "{";
            o += "\"seq\":" + std::to_string(f.seq) + ",";
            o += "\"stream\":" + std::to_string(f.streamIndex) + ",";
            o += "\"dts\":" + jint64(f.dts) + ",";
            o += "\"dtsTime\":" + joptD(f.dtsTime) + ",";
            o += "\"dtsDelta\":" + joptD(f.dtsDelta) + ",";
            o += "\"pts\":" + jint64(f.pts) + ",";
            o += "\"ptsTime\":" + joptD(f.ptsTime) + ",";
            o += "\"duration\":" + std::to_string(f.duration) + ",";
            o += "\"durationTime\":" + joptD(f.durationTime) + ",";
            o += "\"size\":" + std::to_string(f.size) + ",";
            o += std::string("\"key\":") + (f.keyframe ? "true" : "false") + ",";
            o += "\"pictType\":" + joptS(f.pictType) + ",";
            o += "\"samples\":" + joptI64(f.samples) + ",";
            o += std::string("\"decoded\":") + (f.decoded ? "true" : "false") + ",";
            o += "\"width\":" + joptI(f.width) + ",";
            o += "\"height\":" + joptI(f.height) + ",";
            o += "\"pixFmt\":" + joptS(f.pixFmt) + ",";
            o += "\"sampleFmt\":" + joptS(f.sampleFmt) + ",";
            o += "\"channels\":" + joptI(f.channels);
            o += "}";
            if (i + 1 < shown) o += ",";
        }
        o += "]";
    }

    o += ",\"totalPackets\":" + std::to_string(r.totalPacketsRead);
    o += std::string(",\"truncated\":") + (r.truncated ? "true" : "false");
    if (!ffmpegVersion.empty()) o += ",\"ffmpeg\":" + jstr(ffmpegVersion);
    o += "}";
    return o;
}

bool fromJson(const std::string& json, AnalysisResult& r, Diagnostic& d) {
    const JVal root = parseJsonOrNull(json);
    if (!root.isObject()) return false;

    if (const JVal* diag = root.find("diagnostic"); diag && diag->isObject()) {
        if (const JVal* x = diag->find("code")) d.code = errcFromString(x->asString());
        if (const JVal* x = diag->find("stage")) d.stage = stageFromString(x->asString());
        if (const JVal* x = diag->find("native")) d.nativeCode = static_cast<int>(x->asInt());
        if (const JVal* x = diag->find("message")) d.message = x->asString();
        if (const JVal* x = diag->find("hint")) d.hint = x->asString();
        if (const JVal* x = diag->find("url")) d.url = x->asString();
        if (const JVal* x = diag->find("elapsed")) d.elapsedSeconds = x->asDouble();
    }

    if (const JVal* fmt = root.find("format"); fmt && fmt->isObject()) {
        if (const JVal* x = fmt->find("name")) r.formatName = x->asString();
        if (const JVal* x = fmt->find("longName")) r.formatLongName = x->asString();
        if (const JVal* x = fmt->find("duration")) r.duration = x->asDouble();
        if (const JVal* x = fmt->find("bitrate")) r.bitrate = static_cast<int64_t>(x->asInt());
        if (const JVal* x = fmt->find("start")) r.startSeconds = x->asDouble();
    }

    if (const JVal* t = root.find("timings"); t && t->isObject()) {
        auto sec = [&](const char* k) -> double {
            const JVal* x = t->find(k);
            return (!x || x->isNull()) ? -1.0 : x->asDouble();
        };
        r.timings.openSeconds = sec("open");
        r.timings.probeSeconds = sec("probe");
        r.timings.demuxSeconds = sec("demux");
        if (const JVal* x = t->find("total"))
            r.timings.totalSeconds = x->isNull() ? 0.0 : x->asDouble();
    }

    if (const JVal* streams = root.find("streams"); streams && streams->isArray()) {
        for (const auto& sv : streams->arr) {
            if (!sv.isObject()) continue;
            StreamInfo s;
            if (const JVal* x = sv.find("index")) s.index = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("kind")) s.kind = kindFromString(x->asString());
            if (const JVal* x = sv.find("codec")) s.codecName = x->asString();
            if (const JVal* x = sv.find("profile")) s.profile = x->asString();
            if (const JVal* x = sv.find("level")) s.level = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("width")) s.width = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("height")) s.height = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("pixFmt")) s.pixFmt = x->asString();
            if (const JVal* x = sv.find("frameRate")) s.frameRate = x->asDouble();
            if (const JVal* x = sv.find("channels")) s.channels = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("sampleRate")) s.sampleRate = static_cast<int>(x->asInt());
            if (const JVal* x = sv.find("sampleFmt")) s.sampleFmt = x->asString();
            if (const JVal* x = sv.find("timeBase")) s.timeBase = x->asString();
            if (const JVal* x = sv.find("duration")) s.duration = x->asDouble();
            r.streams.push_back(std::move(s));
        }
    }

    if (const JVal* frames = root.find("frames"); frames && frames->isArray()) {
        r.frames.reserve(frames->arr.size());
        for (const auto& fv : frames->arr) {
            if (!fv.isObject()) continue;
            FrameRecord f;
            if (const JVal* x = fv.find("seq")) f.seq = x->asInt();
            if (const JVal* x = fv.find("stream")) f.streamIndex = static_cast<int>(x->asInt());
            if (const JVal* x = fv.find("dts")) f.dts = x->isNull() ? INT64_MIN : x->asInt();
            if (const JVal* x = fv.find("dtsTime"))
                f.dtsTime = x->isNull() ? std::nullopt : std::optional<double>(x->asDouble());
            if (const JVal* x = fv.find("dtsDelta"))
                f.dtsDelta = x->isNull() ? std::nullopt : std::optional<double>(x->asDouble());
            if (const JVal* x = fv.find("pts")) f.pts = x->isNull() ? INT64_MIN : x->asInt();
            if (const JVal* x = fv.find("ptsTime"))
                f.ptsTime = x->isNull() ? std::nullopt : std::optional<double>(x->asDouble());
            if (const JVal* x = fv.find("duration")) f.duration = x->asInt();
            if (const JVal* x = fv.find("durationTime"))
                f.durationTime = x->isNull() ? std::nullopt : std::optional<double>(x->asDouble());
            if (const JVal* x = fv.find("size")) f.size = x->asInt();
            if (const JVal* x = fv.find("key")) f.keyframe = x->asBool();
            if (const JVal* x = fv.find("pictType"))
                f.pictType = x->isNull() ? std::nullopt : std::optional<std::string>(x->asString());
            if (const JVal* x = fv.find("samples"))
                f.samples = x->isNull() ? std::nullopt : std::optional<int64_t>(x->asInt());
            if (const JVal* x = fv.find("decoded")) f.decoded = x->asBool();
            if (const JVal* x = fv.find("width"))
                f.width = x->isNull() ? std::nullopt : std::optional<int>(static_cast<int>(x->asInt()));
            if (const JVal* x = fv.find("height"))
                f.height = x->isNull() ? std::nullopt : std::optional<int>(static_cast<int>(x->asInt()));
            if (const JVal* x = fv.find("pixFmt"))
                f.pixFmt = x->isNull() ? std::nullopt : std::optional<std::string>(x->asString());
            if (const JVal* x = fv.find("sampleFmt"))
                f.sampleFmt = x->isNull() ? std::nullopt : std::optional<std::string>(x->asString());
            if (const JVal* x = fv.find("channels"))
                f.channels = x->isNull() ? std::nullopt : std::optional<int>(static_cast<int>(x->asInt()));
            r.frames.push_back(std::move(f));
        }
    }

    if (const JVal* x = root.find("totalPackets")) r.totalPacketsRead = x->asInt();
    if (const JVal* x = root.find("truncated")) r.truncated = x->asBool();
    return true;
}

std::string toOptionsJson(const AnalysisOptions& o) {
    std::string s = "{";
    s += "\"probesize\":" + std::to_string(o.probesize) + ",";
    s += "\"analyzeduration\":" + std::to_string(o.analyzeduration) + ",";
    s += "\"connectTimeout\":" + jnum(o.connectTimeout) + ",";
    s += "\"stallTimeout\":" + jnum(o.stallTimeout) + ",";
    s += "\"probeTimeout\":" + jnum(o.probeTimeout) + ",";
    s += "\"maxFrames\":" + std::to_string(o.maxFrames) + ",";
    s += std::string("\"decodeFrames\":") + (o.decodeFrames ? "true" : "false") + ",";
    s += "\"maxDecodeFrames\":" + std::to_string(o.maxDecodeFrames) + ",";
    s += std::string("\"keepUnknownStreams\":") + (o.keepUnknownStreams ? "true" : "false") + ",";
    s += "\"extraOptions\":[";
    for (size_t i = 0; i < o.extraOptions.size(); ++i) {
        s += "{\"k\":" + jstr(o.extraOptions[i].first) +
             ",\"v\":" + jstr(o.extraOptions[i].second) + "}";
        if (i + 1 < o.extraOptions.size()) s += ",";
    }
    s += "]";
    s += "}";
    return s;
}

bool fromOptionsJson(const JVal& root, AnalysisOptions& o) {
    if (!root.isObject()) return false;
    if (const JVal* x = root.find("probesize")) o.probesize = x->asInt();
    if (const JVal* x = root.find("analyzeduration")) o.analyzeduration = x->asInt();
    if (const JVal* x = root.find("connectTimeout")) o.connectTimeout = x->asDouble();
    if (const JVal* x = root.find("stallTimeout")) o.stallTimeout = x->asDouble();
    if (const JVal* x = root.find("probeTimeout")) o.probeTimeout = x->asDouble();
    if (const JVal* x = root.find("maxFrames")) o.maxFrames = x->asInt();
    if (const JVal* x = root.find("decodeFrames")) o.decodeFrames = x->asBool();
    if (const JVal* x = root.find("maxDecodeFrames")) o.maxDecodeFrames = x->asInt();
    if (const JVal* x = root.find("keepUnknownStreams")) o.keepUnknownStreams = x->asBool();
    if (const JVal* eo = root.find("extraOptions"); eo && eo->isArray()) {
        for (const auto& e : eo->arr) {
            if (!e.isObject()) continue;
            std::string k, v;
            if (const JVal* x = e.find("k")) k = x->asString();
            if (const JVal* x = e.find("v")) v = x->asString();
            if (!k.empty()) o.extraOptions.emplace_back(k, v);
        }
    }
    return true;
}

bool fromOptionsJson(const std::string& json, AnalysisOptions& o) {
    return fromOptionsJson(parseJsonOrNull(json), o);
}

std::string jsonQuote(const std::string& s) { return jstr(s); }

std::string jsonFieldString(const std::string& json, const std::string& key) {
    const JVal root = parseJsonOrNull(json);
    if (const JVal* x = root.find(key)) return x->asString();
    return {};
}

long long jsonFieldInt(const std::string& json, const std::string& key) {
    const JVal root = parseJsonOrNull(json);
    if (const JVal* x = root.find(key)) return x->asInt();
    return 0;
}

double jsonFieldDouble(const std::string& json, const std::string& key) {
    const JVal root = parseJsonOrNull(json);
    if (const JVal* x = root.find(key)) return x->asDouble();
    return 0.0;
}

bool parseAnalyzeRequest(const std::string& requestJson, std::string& input,
                         AnalysisOptions& opts) {
    const JVal root = parseJsonOrNull(requestJson);
    if (!root.isObject()) return false;
    if (const JVal* x = root.find("input")) input = x->asString();
    if (const JVal* o = root.find("options")) fromOptionsJson(*o, opts);
    return true;
}

} // namespace ma::core
