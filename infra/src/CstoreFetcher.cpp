// ma::infra - cstore recording fetcher. See header for the external contract.
#include "ma/infra/CstoreFetcher.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "ma/core/Serialization.hpp"
#include "ma/infra/Logger.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace ma::infra {
namespace {

constexpr const char* kApiUrl =
    "http://cstore.seewo.com/cstore/api/v2/downloadUrl?isTmp=false&appId=10363";
constexpr const char* kReferer = "http://cstore.seewo.com";
constexpr size_t kMaxApiResponse = 1u * 1024u * 1024u;
constexpr size_t kMaxPlaylistBytes = 4u * 1024u * 1024u;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string stripQuery(const std::string& s) {
    std::string out = s;
    if (auto q = out.find('?'); q != std::string::npos) out.resize(q);
    if (auto h = out.find('#'); h != std::string::npos) out.resize(h);
    return out;
}

std::string dirName(const std::string& key) {
    const auto p = key.find_last_of("/\\");
    return p == std::string::npos ? std::string() : key.substr(0, p);
}

std::string joinKey(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

bool looksLikeTs(const std::string& line) {
    const std::string s = lower(stripQuery(trim(line)));
    return s.size() > 3 && s.substr(s.size() - 3) == ".ts";
}

std::string sanitizeStem(const std::string& key) {
    std::string s = stripQuery(key);
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_')) c = '_';
    }
    while (!s.empty() && s.front() == '.') s.erase(s.begin());
    return s.empty() ? std::string("recording") : s;
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// The downloadUrl response is a small, well-formed object produced by our own
// backend. Extract the first string after `key` without a full JSON library.
std::string jsonStringAfter(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') break;
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += e; break;  // \" \\ \/ \u kept simple (URLs have no \u)
            }
        } else {
            out += c;
        }
    }
    return out;
}

#ifdef _WIN32

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool secure = false;
    bool valid = false;
};

UrlParts crack(const std::string& url) {
    UrlParts out;
    const std::wstring w = toWide(url);
    std::vector<wchar_t> host(w.size() + 1), path(w.size() + 1);
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host.data(); uc.dwHostNameLength = static_cast<DWORD>(host.size());
    uc.lpszUrlPath = path.data();  uc.dwUrlPathLength = static_cast<DWORD>(path.size());
    if (!WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return out;
    out.host.assign(host.data());
    out.path.assign(path.data());
    out.port = uc.nPort;
    out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.valid = !out.host.empty();
    return out;
}

struct HttpReply {
    int status = 0;
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

class Session {
public:
    explicit Session(double timeoutSec) {
        const DWORD ms = static_cast<DWORD>(
            std::clamp(timeoutSec, 1.0, 600.0) * 1000.0);
        h_ = WinHttpOpen(L"media-analyse-cstore/1.0",
                         WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h_) WinHttpSetTimeouts(h_, ms, ms, ms, ms);
        // cstore private URLs are plain https GETs without redirects, but be
        // permissive: a 301/302 should not silently become an empty file.
        if (h_) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(h_, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        }
    }
    ~Session() { if (h_) WinHttpCloseHandle(h_); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    HINTERNET get() const { return h_; }

private:
    HINTERNET h_ = nullptr;
};

HttpReply request(Session& session, const std::string& method,
                  const std::string& url, const std::string& body,
                  size_t maxBytes, const CancelFlag* cancel) {
    HttpReply reply;
    const UrlParts u = crack(url);
    if (!u.valid) { reply.error = "bad cstore URL"; return reply; }

    HINTERNET conn = WinHttpConnect(session.get(), u.host.c_str(), u.port, 0);
    if (!conn) { reply.error = "WinHttpConnect failed"; return reply; }
    HINTERNET req = WinHttpOpenRequest(
        conn, toWide(method).c_str(), u.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        reply.error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(conn);
        return reply;
    }

    std::wstring headers = L"Referer: " + toWide(kReferer) + L"\r\n";
    if (method == "POST")
        headers += L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(req, headers.c_str(),
                             static_cast<DWORD>(headers.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    void* bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA
                                 : const_cast<char*>(body.data());
    const DWORD bodyLen = static_cast<DWORD>(body.size());
    const bool sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         bodyPtr, bodyLen, bodyLen, 0);
    if (!sent) {
        reply.error = "WinHttpSendRequest failed (errno=" +
                      std::to_string(GetLastError()) + ")";
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        reply.error = "WinHttpReceiveResponse failed";
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        reply.status = static_cast<int>(status);
        std::string chunk(16384, '\0');
        while (reply.body.size() < maxBytes) {
            if (cancel && cancel->load()) { reply.error = "cancelled"; break; }
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), static_cast<DWORD>(chunk.size()), &got))
                { reply.error = "WinHttpReadData failed"; break; }
            if (got == 0) break;
            const size_t take = std::min<size_t>(got, maxBytes - reply.body.size());
            reply.body.append(chunk.data(), take);
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return reply;
}

std::string resolveDownloadUrl(Session& session, const std::string& key,
                               double /*timeoutSec*/, const CancelFlag* cancel,
                               std::string* error) {
    const std::string body = "[" + ma::core::jsonQuote(key) + "]";
    const HttpReply r = request(session, "POST", kApiUrl, body,
                                kMaxApiResponse, cancel);
    if (!r.ok()) {
        *error = "downloadUrl API " + std::to_string(r.status) + ": " +
                 (r.error.empty() ? r.body.substr(0, 160) : r.error);
        return {};
    }
    const std::string url = jsonStringAfter(r.body, "downloadUrl");
    if (url.rfind("http", 0) != 0) {
        *error = "downloadUrl API returned no usable URL";
        return {};
    }
    return url;
}

struct PlaylistRead {
    std::string text;
    std::string error;
};

PlaylistRead readPlaylist(Session& session, const std::string& key,
                          double timeoutSec, const CancelFlag* cancel) {
    PlaylistRead out;
    std::string err;
    const std::string url = resolveDownloadUrl(session, key, timeoutSec, cancel, &err);
    if (url.empty()) { out.error = err; return out; }
    const HttpReply r = request(session, "GET", url, {}, kMaxPlaylistBytes, cancel);
    if (!r.ok()) {
        out.error = "playlist HTTP " + std::to_string(r.status) + ": " +
                    (r.error.empty() ? r.body.substr(0, 160) : r.error);
        return out;
    }
    if (r.body.find("<Error>") != std::string::npos) {
        out.error = "playlist response is an OSS <Error> document";
        return out;
    }
    out.text = r.body;
    return out;
}

struct SegmentTask {
    int index = -1;
    std::string objectKey;
};

void downloadSegment(const SegmentTask& task, FetchedSegment& result,
                     const std::string& dir, double timeoutSec,
                     const CancelFlag* cancel) {
    result.index = task.index;
    result.objectKey = task.objectKey;
    {
        char b[32];
        std::snprintf(b, sizeof(b), "segment_%03d.ts", task.index);
        result.localName = b;
    }
    // Remove any stale file from a previous run FIRST: a failed re-download
    // must leave a real hole in the local playlist, not silently reuse an old
    // segment and hide the recording defect.
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(dir) / result.localName, ec);
    }
    if (cancel && cancel->load()) { result.error = "cancelled"; return; }

    Session session(timeoutSec);
    std::string err;
    const std::string url =
        resolveDownloadUrl(session, task.objectKey, timeoutSec, cancel, &err);
    if (url.empty()) { result.error = err; return; }

    const std::filesystem::path tmp = std::filesystem::path(dir) /
        (result.localName + ".part");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { result.error = "cannot create " + tmp.string(); return; }
        // Stream straight to disk instead of buffering a whole segment.
        HttpReply r;
        const UrlParts u = crack(url);
        if (!u.valid) { result.error = "bad segment URL"; return; }
        HINTERNET conn = WinHttpConnect(session.get(), u.host.c_str(), u.port, 0);
        if (!conn) { result.error = "WinHttpConnect failed for segment"; return; }
        HINTERNET req = WinHttpOpenRequest(
            conn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            result.error = "WinHttpOpenRequest failed for segment";
            WinHttpCloseHandle(conn);
            return;
        }
        const std::wstring hdr = L"Referer: " + toWide(kReferer) + L"\r\n";
        WinHttpAddRequestHeaders(req, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        bool ok = false;
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            r.status = static_cast<int>(status);
            if (r.ok()) {
                std::string buf(32768, '\0');
                ok = true;
                while (true) {
                    if (cancel && cancel->load()) { ok = false; r.error = "cancelled"; break; }
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf.data(),
                                         static_cast<DWORD>(buf.size()), &got)) {
                        ok = false; r.error = "WinHttpReadData failed"; break;
                    }
                    if (got == 0) break;
                    out.write(buf.data(), static_cast<std::streamsize>(got));
                    result.bytes += got;
                }
            } else {
                r.error = "HTTP " + std::to_string(r.status);
            }
        } else {
            r.error = "segment request failed";
        }
        if (!ok) result.error = r.error.empty() ? "download failed" : r.error;
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
    }
    if (!result.error.empty()) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    } else {
        std::error_code ec;
        const auto finalPath = std::filesystem::path(dir) / result.localName;
        std::filesystem::remove(finalPath, ec);   // idempotent re-runs
        ec.clear();
        std::filesystem::rename(tmp, finalPath, ec);
        if (ec) {
            result.error = "cannot finalize segment file: " + ec.message();
            std::filesystem::remove(tmp, ec);
        } else {
            result.ok = true;
        }
    }
}

#endif // _WIN32

} // namespace

Outcome<FetchReport> fetchCstoreRecording(const std::string& rawKey,
                                          const AnalysisOptions& options,
                                          const CancelFlag* cancel,
                                          const FetchProgressFn& onProgress) {
    Outcome<FetchReport> out;
    FetchReport& rep = out.value;
    rep.objectKey = trim(rawKey);
    const auto started = std::chrono::steady_clock::now();

    Diagnostic& diag = out.diag;
    diag.stage = Stage::Open;
    diag.url = rep.objectKey;
    if (rep.objectKey.empty()) {
        diag.code = Errc::InvalidArgument;
        diag.message = "empty cstore object key";
        return out;
    }
    // Accept a full URL pasted by mistake: take only the path part.
    if (rep.objectKey.rfind("http", 0) == 0) {
        const auto schemeEnd = rep.objectKey.find("://");
        if (schemeEnd != std::string::npos) {
            const auto slash = rep.objectKey.find('/', schemeEnd + 3);
            rep.objectKey = slash == std::string::npos
                                ? std::string()
                                : rep.objectKey.substr(slash + 1);
        }
    }
    if (rep.objectKey.empty()) {
        diag.code = Errc::InvalidArgument;
        diag.message = "cannot derive cstore key from URL";
        return out;
    }

#ifndef _WIN32
    diag.code = Errc::UnsupportedScheme;
    diag.message = "cstore fetcher requires WinHTTP (Windows only)";
    return out;
#else
    const double timeout = std::clamp(options.fetchTimeout, 1.0, 600.0);

    const bool explicitDir = !options.fetchDir.empty();
    std::filesystem::path dir = explicitDir
        ? std::filesystem::path(options.fetchDir)
        : std::filesystem::current_path() / "fetched" / sanitizeStem(rep.objectKey);
    std::error_code ec;
    if (!std::filesystem::create_directories(dir, ec) && ec) {
        if (explicitDir) {
            diag.code = Errc::InvalidArgument;
            diag.message = "cannot create fetch directory: " + ec.message();
            return out;
        }
        // A deep caller cwd (e.g. a GUI opened from a long path) pushes
        // <cwd>/fetched/<key> past Windows MAX_PATH. Fall back to a short,
        // stable location under the system temp dir instead of failing.
        const std::filesystem::path fallback =
            std::filesystem::temp_directory_path() / "MediaAnalyse" /
            "fetched" / sanitizeStem(rep.objectKey);
        ec.clear();
        if (!std::filesystem::create_directories(fallback, ec) && !ec) dir = fallback;
        if (ec) {
            diag.code = Errc::InvalidArgument;
            diag.message = "cannot create fetch directory: " + ec.message();
            return out;
        }
    }
    rep.directory = pathToUtf8(std::filesystem::absolute(dir));

    Session session(timeout);
    if (!session.get()) {
        diag.code = Errc::Internal;
        diag.message = "WinHttpOpen failed";
        return out;
    }
    if (onProgress) onProgress(0, 0, "resolve-playlist");

    PlaylistRead pl = readPlaylist(session, rep.objectKey, timeout, cancel);
    if (pl.error.empty()) {
        // Master playlist: pick the first variant, as the reference script's
        // users do manually. Relative URIs resolve against the playlist dir.
        bool master = false;
        std::string variant;
        std::istringstream in(pl.text);
        std::string line;
        bool expectVariant = false;
        while (std::getline(in, line)) {
            line = trim(line);
            if (lower(line).rfind("#ext-x-stream-inf", 0) == 0) {
                master = true;
                expectVariant = true;
                continue;
            }
            if (expectVariant && !line.empty() && line.front() != '#') {
                variant = line;
                break;
            }
        }
        if (master) {
            if (variant.empty()) {
                diag.code = Errc::InvalidArgument;
                diag.message = "master playlist has no variant";
                return out;
            }
            rep.isVariant = true;
            rep.playlistKey = variant.find("://") != std::string::npos
                                  ? variant
                                  : joinKey(dirName(rep.objectKey), stripQuery(variant));
            pl = readPlaylist(session, rep.playlistKey, timeout, cancel);
        } else {
            rep.playlistKey = rep.objectKey;
        }
    }
    if (!pl.error.empty()) {
        diag.code = Errc::FfmpegError;
        diag.message = pl.error;
        diag.hint = "check that the key exists and the cstore downloadUrl API is reachable";
        return out;
    }

    // Build the local playlist: every TS line is replaced by its local name,
    // including the ones that later fail to download. That preserves the
    // index holes needed for segment-missing diagnostics.
    std::vector<SegmentTask> tasks;
    std::vector<std::string> localLines;
    {
        std::istringstream in(pl.text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (looksLikeTs(line)) {
                const int idx = static_cast<int>(tasks.size());
                const std::string name = stripQuery(trim(line));
                tasks.push_back({idx, joinKey(dirName(rep.playlistKey), name)});
                char b[16];
                std::snprintf(b, sizeof(b), "segment_%03d.ts", idx);
                localLines.push_back(b);
            } else {
                localLines.push_back(line);
            }
        }
    }
    if (tasks.empty()) {
        diag.code = Errc::InvalidArgument;
        diag.message = "playlist contains no TS segments";
        return out;
    }
    rep.segmentsTotal = static_cast<int>(tasks.size());
    rep.segments.resize(tasks.size());
    if (onProgress) onProgress(0, rep.segmentsTotal, "download");

    const int jobs = std::clamp(options.fetchJobs, 1, 16);
    std::atomic<int> next{0};
    std::atomic<int> done{0};
    std::mutex progressMu;
    auto worker = [&]() {
        while (true) {
            if (cancel && cancel->load()) break;
            const int i = next.fetch_add(1);
            if (i >= static_cast<int>(tasks.size())) break;
            downloadSegment(tasks[static_cast<size_t>(i)],
                            rep.segments[static_cast<size_t>(i)],
                            rep.directory, timeout, cancel);
            const int n = ++done;
            if (onProgress) {
                std::lock_guard<std::mutex> lk(progressMu);
                onProgress(n, rep.segmentsTotal, "download");
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(jobs));
    for (int j = 0; j < jobs; ++j) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    for (const auto& s : rep.segments) {
        if (s.ok) ++rep.segmentsDownloaded;
        else ++rep.segmentsFailed;
    }

    const std::filesystem::path playlistPath =
        std::filesystem::path(rep.directory) / "local.m3u8";
    {
        std::ofstream f(playlistPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            diag.code = Errc::InvalidArgument;
            diag.message = "cannot write local playlist";
            return out;
        }
        for (const auto& l : localLines) f << l << "\n";
    }
    rep.localPlaylist = pathToUtf8(std::filesystem::absolute(playlistPath));

    if (cancel && cancel->load()) {
        diag.code = Errc::Cancelled;
        diag.stage = Stage::Cancelled;
        diag.message = "cstore fetch cancelled";
        return out;
    }
    if (rep.segmentsDownloaded == 0) {
        diag.code = Errc::FfmpegError;
        diag.message = "no cstore segment could be downloaded";
        diag.hint = rep.segments.empty() ? "" : rep.segments.front().error;
        return out;
    }
    diag.code = Errc::Ok;
    diag.stage = Stage::None;
    diag.message.clear();
    diag.url = rep.localPlaylist;
    rep.elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return out;
#endif
}

std::string describeFetch(const FetchReport& report) {
    std::string s = "cstore: " + std::to_string(report.segmentsTotal) + " segments, " +
                    std::to_string(report.segmentsDownloaded) + " downloaded, " +
                    std::to_string(report.segmentsFailed) + " missing";
    if (report.segmentsFailed > 0) {
        s += " (";
        int shown = 0;
        for (const auto& seg : report.segments) {
            if (seg.ok || shown >= 12) continue;
            if (shown) s += ", ";
            char b[16];
            std::snprintf(b, sizeof(b), "%03d", seg.index);
            s += b;
            ++shown;
        }
        if (report.segmentsFailed > shown) s += ", ...";
        s += ")";
    }
    if (report.isVariant) s += "; master playlist -> first variant";
    return s;
}

} // namespace ma::infra
