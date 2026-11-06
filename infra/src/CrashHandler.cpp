// ma::infra - Windows minidump crash handler. See CrashHandler.hpp.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "ma/infra/CrashHandler.hpp"

namespace ma::infra {
namespace {

std::string g_appName = "app";
std::string g_dumpDir;
std::string g_lastDump;

// Best-effort timestamp "YYYYMMDD-HHMMSS".
std::string timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string moduleDir() {
    char mod[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, mod, MAX_PATH);
    if (n == 0) return ".";
    std::string s(mod, n);
    auto pos = s.find_last_of("/\\");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

void ensureDumpDir() {
    if (!g_dumpDir.empty()) return;
    g_dumpDir = moduleDir() + "\\crashes";
    CreateDirectoryA(g_dumpDir.c_str(), nullptr);  // ignore exists/error
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    ensureDumpDir();
    std::string path = g_dumpDir + "\\" + g_appName + "-" + timestamp() + ".dmp";
    g_lastDump = path;

    HANDLE hFile =
        CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
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
    }
    // Surface the path on stderr so a supervising GUI can echo it even if it
    // can't read the dump directory directly.
    std::fprintf(stderr, "\n[CRASHDUMP] %s\n", path.c_str());
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// C-runtime failure modes.
void invalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    std::fprintf(stderr, "\n[CRASH] invalid parameter\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
}
void purecall() {
    std::fprintf(stderr, "\n[CRASH] pure virtual call\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
}
void onTerminate() {
    std::fprintf(stderr, "\n[CRASH] std::terminate\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 5);
}

} // namespace

void installCrashHandler(const char* appName) {
    if (appName && *appName) g_appName = appName;
    ensureDumpDir();
    SetUnhandledExceptionFilter(unhandledFilter);
    _set_invalid_parameter_handler(invalidParameter);
    _set_purecall_handler(purecall);
    std::set_terminate(onTerminate);
}

const char* lastCrashDumpPath() { return g_lastDump.c_str(); }

} // namespace ma::infra
#else
// Non-Windows: nothing to do.
namespace ma::infra {
void installCrashHandler(const char*) {}
const char* lastCrashDumpPath() { return ""; }
} // namespace ma::infra
#endif
