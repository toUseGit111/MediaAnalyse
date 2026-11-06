// ma::infra - Windows minidump crash handler. See CrashHandler.hpp.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Logger.hpp"

namespace ma::infra {
namespace {

std::string g_appName = "app";
std::wstring g_dumpDir;
std::string g_dumpDirUtf8;
std::string g_lastDump;

std::string utf8FromWide(const std::wstring& w) {
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

std::wstring wideFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::wstring getEnvWide(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    if (got == 0 || got >= n) return {};
    v.resize(got);
    return v;
}

std::wstring moduleDirWide() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return L".";
        if (n < buf.size() - 1) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const auto pos = buf.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".")
                                     : buf.substr(0, pos);
}

// Best-effort timestamp "YYYYMMDD-HHMMSS".
std::string timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

bool tryMakeWritable(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    const auto probe = dir / L".ma-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char byte = 'x';
    DWORD written = 0;
    const BOOL ok = WriteFile(h, &byte, 1, &written, nullptr);
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return ok && written == 1;
}

void ensureDumpDir() {
    if (!g_dumpDir.empty()) return;
    // Mirror the logger's directory policy: prefer next to the exe (easiest to
    // find in a green package), fall back to a per-user location when the
    // install directory is read-only (e.g. Program Files).
    std::wstring local = getEnvWide(L"LOCALAPPDATA");
    std::wstring temp = getEnvWide(L"TEMP");
    if (temp.empty()) temp = getEnvWide(L"TMP");
    std::filesystem::path chosen;
    // The logger already solved "where can this process write?". Keep dumps in
    // the sibling directory so a support bundle collects both from one place.
    const std::string logDir = logDirectory();
    if (!logDir.empty()) {
        const std::filesystem::path sibling =
            std::filesystem::path(wideFromUtf8(logDir)).parent_path() / L"crashes";
        if (tryMakeWritable(sibling)) chosen = sibling;
    }
    const std::filesystem::path candidates[] = {
        std::filesystem::path(moduleDirWide()) / L"crashes",
        local.empty() ? std::filesystem::path()
                      : std::filesystem::path(local) / L"MediaAnalyse" / L"crashes",
        temp.empty() ? std::filesystem::path()
                     : std::filesystem::path(temp) / L"MediaAnalyse" / L"crashes",
    };
    for (const auto& c : candidates) {
        if (chosen.empty() && !c.empty() && tryMakeWritable(c)) {
            chosen = c;
            break;
        }
    }
    if (chosen.empty()) return;  // nowhere writable: still log the fault
    g_dumpDir = chosen.wstring();
    g_dumpDirUtf8 = utf8FromWide(g_dumpDir);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    const unsigned long code =
        (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    char codeBuf[32];
    std::snprintf(codeBuf, sizeof(codeBuf), "0x%08lX", code);
    emergencyLog("crash", std::string("unhandled exception ") + codeBuf);

    ensureDumpDir();
    std::string path;
    if (!g_dumpDir.empty()) {
        const std::string name =
            g_appName + "-" + timestamp() + "-" + std::to_string(GetCurrentProcessId());
        const std::wstring full =
            g_dumpDir + L"\\" + wideFromUtf8(name) + L".dmp";
        path = utf8FromWide(full);
        g_lastDump = path;

        HANDLE hFile = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                              (MINIDUMP_TYPE)(MiniDumpWithDataSegs |
                                              MiniDumpWithProcessThreadData),
                              &mei, nullptr, nullptr);
            CloseHandle(hFile);
            emergencyLog("crash", "minidump written: " + path);
        } else {
            emergencyLog("crash", "FAILED to create minidump in " + g_dumpDirUtf8);
        }
    } else {
        emergencyLog("crash", "no writable crash directory; minidump skipped");
    }

    // Surface the path on stderr so a supervising GUI can echo it even if it
    // can't read the dump directory directly.
    if (!path.empty()) {
        std::fprintf(stderr, "\n[CRASHDUMP] %s\n", path.c_str());
        std::fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// C-runtime failure modes.
void invalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    emergencyLog("crash", "invalid parameter handler - terminating");
    std::fprintf(stderr, "\n[CRASH] invalid parameter\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
}
void purecall() {
    emergencyLog("crash", "pure virtual call - terminating");
    std::fprintf(stderr, "\n[CRASH] pure virtual call\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
}
void onTerminate() {
    emergencyLog("crash", "std::terminate - terminating");
    std::fprintf(stderr, "\n[CRASH] std::terminate\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 5);
}

}  // namespace

void installCrashHandler(const char* appName) {
    if (appName && *appName) g_appName = appName;
    ensureDumpDir();
    SetUnhandledExceptionFilter(unhandledFilter);
    _set_invalid_parameter_handler(invalidParameter);
    _set_purecall_handler(purecall);
    std::set_terminate(onTerminate);
    MA_LOGI("crash", "handler installed; dump dir="
                         << (g_dumpDirUtf8.empty() ? "<unavailable>" : g_dumpDirUtf8));
}

const char* lastCrashDumpPath() { return g_lastDump.c_str(); }

}  // namespace ma::infra
#else
// Non-Windows: nothing to do.
namespace ma::infra {
void installCrashHandler(const char*) {}
const char* lastCrashDumpPath() { return ""; }
}  // namespace ma::infra
#endif
