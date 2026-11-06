// ma::infra - file logging implementation. See Logger.hpp for the contract.
#include "ma/infra/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ma::infra {
namespace {

std::mutex g_mutex;
std::filesystem::path g_path;
std::string g_pathUtf8;
std::string g_dirUtf8;
Severity g_min = Severity::Info;
long long g_maxBytes = 5LL * 1024 * 1024;
int g_maxFiles = 5;
bool g_enabled = false;

#ifdef _WIN32
HANDLE g_handle = INVALID_HANDLE_VALUE;
#else
std::FILE* g_file = nullptr;
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::string pathToUtf8(const fs::path& p) { return wideToUtf8(p.wstring()); }

std::wstring getEnvWide(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    if (got == 0 || got >= n) return {};
    v.resize(got);
    return v;
}

fs::path moduleDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return fs::path(L".");
        if (n < buf.size() - 1) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const auto pos = buf.find_last_of(L"\\/");
    return fs::path(pos == std::wstring::npos ? L"." : buf.substr(0, pos));
}

// Verify we can actually create AND write a file here (installing under
// Program Files makes the exe directory read-only for standard users).
bool writableDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / L".ma-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char byte = 'x';
    DWORD written = 0;
    const BOOL ok = WriteFile(h, &byte, 1, &written, nullptr);
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return ok && written == 1;
}
#else
std::string pathToUtf8(const fs::path& p) { return p.string(); }

fs::path moduleDir() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(".") : p.parent_path();
}

bool writableDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / ".ma-write-test";
    std::FILE* f = std::fopen(probe.string().c_str(), "wb");
    if (!f) return false;
    std::fputc('x', f);
    std::fclose(f);
    std::remove(probe.string().c_str());
    return true;
}
#endif

const char* levelTag(Severity s) {
    switch (s) {
    case Severity::Debug: return "DEBUG";
    case Severity::Info: return "INFO ";
    case Severity::Warning: return "WARN ";
    case Severity::Error: return "ERROR";
    case Severity::Fatal: return "FATAL";
    default: return "     ";
    }
}

Severity parseLevel(const char* v) {
    if (!v || !*v) return Severity::Info;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "debug" || s == "trace" || s == "verbose") return Severity::Debug;
    if (s == "info") return Severity::Info;
    if (s == "warn" || s == "warning") return Severity::Warning;
    if (s == "error" || s == "fatal") return Severity::Error;
    if (s == "off" || s == "none" || s == "0") return Severity::Off;
    return Severity::Info;
}

long long envInt(const char* name, long long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    return (end && end != v && n > 0) ? n : fallback;
}

// Keep every record on exactly one line, and bound pathological av_log output.
std::string sanitize(const std::string& in) {
    constexpr size_t kMax = 4096;
    std::string out;
    out.reserve(std::min(in.size(), kMax) + 1);
    for (size_t i = 0; i < in.size() && i < kMax; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '\r' || c == '\n' || c == '\t') out += ' ';
        else if (c < 0x20 || c == 0x7f) out += '.';
        else out += static_cast<char>(c);
    }
    if (in.size() > kMax) out += "... [truncated]";
    return out;
}

std::string formatLine(Severity sev, const char* component,
                       const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);  // MSVC and MinGW-w64
#else
    localtime_r(&t, &tm);  // POSIX
#endif
    char ts[64];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
    const unsigned long tid = GetCurrentThreadId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
    const unsigned long tid = 0;
#endif
    char head[160];
    std::snprintf(head, sizeof(head), "%s [%s] [p%lu/t%lu] [%s] ", ts,
                  levelTag(sev), pid, tid, component ? component : "app");
    return std::string(head) + sanitize(message);
}

#ifdef _WIN32
fs::path rotatedPath(int index) {
    const std::wstring name = g_path.stem().wstring() + L"." +
                              std::to_wstring(index) +
                              g_path.extension().wstring();
    return g_path.parent_path() / name;
}

bool openHandle() {
    g_handle = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_handle != INVALID_HANDLE_VALUE;
}

void rotateIfNeeded() {
    LARGE_INTEGER size{};
    if (g_handle == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(g_handle, &size) || size.QuadPart < g_maxBytes)
        return;
    CloseHandle(g_handle);
    g_handle = INVALID_HANDLE_VALUE;
    // Oldest first; failures (another process holding the file) are ignored -
    // we simply keep appending rather than lose diagnostics.
    for (int i = g_maxFiles - 1; i >= 1; --i) {
        const fs::path from = rotatedPath(i);
        if (fs::exists(from)) {
            DeleteFileW(rotatedPath(i + 1).c_str());
            MoveFileExW(from.c_str(), rotatedPath(i + 1).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        }
    }
    DeleteFileW(rotatedPath(1).c_str());
    MoveFileExW(g_path.c_str(), rotatedPath(1).c_str(),
                MOVEFILE_REPLACE_EXISTING);
    openHandle();
}
#else
bool openHandle() {
    g_file = std::fopen(g_path.string().c_str(), "a");
    return g_file != nullptr;
}

fs::path rotatedPath(int index) {
    return g_path.parent_path() /
           (g_path.stem().string() + "." + std::to_string(index) +
            g_path.extension().string());
}

void rotateIfNeeded() {
    if (!g_file) return;
    std::error_code ec;
    const auto size = fs::file_size(g_path, ec);
    if (ec || static_cast<long long>(size) < g_maxBytes) return;
    std::fclose(g_file);
    g_file = nullptr;
    for (int i = g_maxFiles - 1; i >= 1; --i) {
        std::error_code ec;
        if (fs::exists(rotatedPath(i))) {
            fs::rename(rotatedPath(i), rotatedPath(i + 1), ec);
        }
    }
    std::error_code ec;
    fs::rename(g_path, rotatedPath(1), ec);
    openHandle();
}
#endif

// Caller holds g_mutex.
void writeLocked(Severity sev, const char* component, const std::string& message) {
    if (!g_enabled || sev < g_min || g_min == Severity::Off) return;
    rotateIfNeeded();
    const std::string line = formatLine(sev, component, message) + "\n";
#ifdef _WIN32
    if (g_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_handle, line.data(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
    }
#else
    if (g_file) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
    }
#endif
}

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string toLowerAscii(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](char c) { return lowerAscii(c); });
    return o;
}

}  // namespace

std::string redact(const std::string& text) {
    std::string s = text;
    if (s.find("://") == std::string::npos && s.find('?') == std::string::npos)
        return s;

    // 1) scheme://user:pass@host -> scheme://***@host
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        const size_t hostStart = scheme + 3;
        const size_t pathStart = s.find_first_of("/?#", hostStart);
        const size_t at = s.find('@', hostStart);
        if (at != std::string::npos &&
            (pathStart == std::string::npos || at < pathStart)) {
            s.replace(hostStart, at + 1 - hostStart, "***@");
        }
    }

    // 2) sensitive query parameters. Scan with a moving offset so every
    //    occurrence is masked even though replacements change the length.
    static const char* kKeys[] = {"access_token", "token", "password", "passwd",
                                  "pwd", "secret", "signature", "sig", "auth",
                                  "api_key", "apikey", "key", "session"};
    for (const char* key : kKeys) {
        const std::string needle = std::string(key) + "=";
        size_t scan = 0;
        for (int guard = 0; guard < 64; ++guard) {
            const std::string lower = toLowerAscii(s);
            const size_t at = lower.find(needle, scan);
            if (at == std::string::npos) break;
            const bool isParam =
                at == 0 || lower[at - 1] == '?' || lower[at - 1] == '&';
            const size_t valueStart = at + needle.size();
            size_t valueEnd = valueStart;
            while (valueEnd < s.size() && s[valueEnd] != '&' &&
                   s[valueEnd] != ' ' && s[valueEnd] != '"' &&
                   s[valueEnd] != '\'' && s[valueEnd] != '\r' &&
                   s[valueEnd] != '\n')
                ++valueEnd;
            if (!isParam || valueEnd == valueStart) {
                scan = std::max(valueStart, scan + 1);
                continue;
            }
            s.replace(valueStart, valueEnd - valueStart, "***");
            scan = valueStart + 3;  // strlen("***")
        }
    }
    return s;
}

bool initLogger(const char* appName) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_enabled) return true;

    g_min = parseLevel(std::getenv("MA_LOG_LEVEL"));
    g_maxBytes = envInt("MA_LOG_MAX_BYTES", g_maxBytes);
    g_maxFiles = static_cast<int>(envInt("MA_LOG_MAX_FILES", g_maxFiles));

    std::string name = (appName && *appName) ? appName : "app";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '.' ||
            c < 0x20)
            c = '_';
    }

    std::vector<fs::path> candidates;
#ifdef _WIN32
    if (const char* explicitDir = std::getenv("MA_LOG_DIR")) {
        const std::wstring w = utf8ToWide(explicitDir);
        if (!w.empty()) candidates.emplace_back(w);
    }
    candidates.push_back(moduleDir() / L"logs");
    const std::wstring local = getEnvWide(L"LOCALAPPDATA");
    if (!local.empty()) candidates.push_back(fs::path(local) / L"MediaAnalyse" / L"logs");
    std::wstring temp = getEnvWide(L"TEMP");
    if (temp.empty()) temp = getEnvWide(L"TMP");
    if (!temp.empty()) candidates.push_back(fs::path(temp) / L"MediaAnalyse" / L"logs");
#else
    if (const char* explicitDir = std::getenv("MA_LOG_DIR"))
        candidates.emplace_back(explicitDir);
    candidates.push_back(moduleDir() / "logs");
    if (const char* home = std::getenv("HOME"))
        candidates.push_back(fs::path(home) / ".local" / "share" / "MediaAnalyse" / "logs");
    candidates.push_back(fs::temp_directory_path() / "MediaAnalyse" / "logs");
#endif

    // Explicitly disabled (e.g. a 10k-iteration fuzz run): resolve display
    // paths but create NO directory and open NO file, so the harness stays as
    // fast as it was before logging existed.
    if (g_min == Severity::Off) {
        if (!candidates.empty()) {
            g_path = candidates.front() / (name + ".log");
            g_pathUtf8 = pathToUtf8(g_path);
            g_dirUtf8 = pathToUtf8(candidates.front());
        }
        return false;
    }

    fs::path chosen;
    for (const auto& dir : candidates) {
        if (writableDir(dir)) {
            chosen = dir;
            break;
        }
    }
    if (chosen.empty()) return false;  // silent no-op; never break the product

    g_path = chosen / (name + ".log");
    g_pathUtf8 = pathToUtf8(g_path);
    g_dirUtf8 = pathToUtf8(chosen);
#ifdef _WIN32
    // MA_LOG_DIR may arrive with forward slashes while path concatenation adds
    // backslashes; present one consistent form in the log and in the UI.
    std::replace(g_pathUtf8.begin(), g_pathUtf8.end(), '/', '\\');
    std::replace(g_dirUtf8.begin(), g_dirUtf8.end(), '/', '\\');
#endif
    if (!openHandle()) return false;
    g_enabled = true;

    const char* levelName = "info";
    switch (g_min) {
    case Severity::Debug: levelName = "debug"; break;
    case Severity::Info: levelName = "info"; break;
    case Severity::Warning: levelName = "warn"; break;
    case Severity::Error: levelName = "error"; break;
    case Severity::Fatal: levelName = "fatal"; break;
    case Severity::Off: levelName = "off"; break;
    }

    // Banner: everything a support ticket needs before the first operation.
    writeLocked(Severity::Info, "logger",
                "===== " + name + " session start =====");
    writeLocked(Severity::Info, "logger", "log file: " + g_pathUtf8);
    writeLocked(Severity::Info, "logger",
                std::string("level=") + levelName +
                ", rotate=" + std::to_string(g_maxBytes) + " bytes x " +
                std::to_string(g_maxFiles));
    return true;
}

bool loggerEnabled() { return g_enabled; }
Severity logLevel() { return g_min; }
std::string logFilePath() { return g_pathUtf8; }
std::string logDirectory() { return g_dirUtf8; }

void logMessage(Severity sev, const char* component, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    writeLocked(sev, component, message);
}

void emergencyLog(const char* component, const std::string& message) {
    if (g_path.empty()) return;
    const std::string line =
        formatLine(Severity::Fatal, component, message) + "\n";
#ifdef _WIN32
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
#else
    if (std::FILE* f = std::fopen(g_path.string().c_str(), "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }
#endif
}

void closeLogger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled) return;
#ifdef _WIN32
    if (g_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_handle);
        g_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
#endif
    g_enabled = false;
}

}  // namespace ma::infra
